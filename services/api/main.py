import os
import sqlite3
from collections.abc import AsyncIterator, Generator
from contextlib import asynccontextmanager
from datetime import datetime, timedelta
from time import time
from typing import Annotated, Literal
from zoneinfo import ZoneInfo

from fastapi import Depends, FastAPI, HTTPException
from fastapi.middleware.gzip import GZipMiddleware
from loguru import logger
from pydantic import BaseModel

DATABASE_PATH = "/data/middlines.db"
TIMEZONE = ZoneInfo(os.environ.get("TZ", "America/New_York"))

# Cache TTL in seconds
CACHE_TTL = 30

# Trend: compare current count to N rows back
TREND_LOOKBACK_ROWS = 5
# Trend: percentage change threshold to determine increasing/decreasing
TREND_THRESHOLD = 0.07

# Aggregation: time bucket size in minutes
TIME_BUCKET_SIZE = 10
# Aggregation: days of historical data to consider
LOOKBACK_DAYS = 45
# Aggregation: percentile for max count calculation (0.99 = 99th percentile)
MAX_PERCENTILE = 0.99
# Aggregation: multiplier of baseline below which location is considered closed
CLOSED_THRESHOLD = 1.5


# Response Models
class DataPoint(BaseModel):
    timestamp: datetime
    busyness_percentage: float | None


class LocationStatus(BaseModel):
    location: str
    timestamp: datetime
    busyness_percentage: float | None
    vs_typical_percentage: float | None
    trend: Literal["Increasing", "Steady", "Decreasing"] | None
    today_data: list[DataPoint]


# Internal Models
class SmoothedCount(BaseModel):
    location: str
    timestamp: datetime
    count: float


class LocationAggregates(BaseModel):
    baseline: float
    max_count: float  # baseline-adjusted
    time_averages: dict[tuple[int, int], float]  # (day_of_week, time_bucket) -> mean


def _compute_aggregates(
    counts: list[SmoothedCount],
) -> dict[str, LocationAggregates]:
    now = datetime.now(TIMEZONE)
    yesterday = now - timedelta(days=1)
    lookback_start = now - timedelta(days=LOOKBACK_DAYS)

    # Group counts by location
    by_location: dict[str, list[SmoothedCount]] = {}
    for c in counts:
        by_location.setdefault(c.location, []).append(c)

    result: dict[str, LocationAggregates] = {}

    for location, location_counts in by_location.items():
        # 1. Baseline: average of 1-4 AM readings from last 24 hours
        baseline_counts = [
            c.count
            for c in location_counts
            if c.timestamp > yesterday and c.timestamp.hour in set(range(1, 4))
        ]
        baseline = sum(baseline_counts) / len(baseline_counts) if baseline_counts else 0

        # 2. Max count: 99th percentile of counts above closed threshold
        open_counts = sorted(
            c.count
            for c in location_counts
            if c.timestamp > lookback_start and c.count > baseline * CLOSED_THRESHOLD
        )
        if open_counts:
            percentile_idx = int(len(open_counts) * MAX_PERCENTILE)
            percentile_idx = min(percentile_idx, len(open_counts) - 1)
            max_count = open_counts[percentile_idx] - baseline
        else:
            max_count = 0

        # 3. Time averages: mean count by (day_of_week, time_bucket)
        time_buckets: dict[tuple[int, int], list[float]] = {}
        for c in location_counts:
            if c.timestamp <= lookback_start:
                continue
            if c.count <= baseline * CLOSED_THRESHOLD:
                continue
            day_of_week = c.timestamp.weekday()
            # Convert to Sunday=0 format to match original SQL strftime('%w')
            day_of_week = (day_of_week + 1) % 7
            minutes = (
                c.timestamp.hour * 60
                + (c.timestamp.minute // TIME_BUCKET_SIZE) * TIME_BUCKET_SIZE
            )
            key = (day_of_week, minutes)
            time_buckets.setdefault(key, []).append(c.count)

        time_averages = {k: sum(v) / len(v) for k, v in time_buckets.items()}

        result[location] = LocationAggregates(
            baseline=baseline,
            max_count=max_count,
            time_averages=time_averages,
        )

    return result


def _calculate_busyness(
    count: float | None,
    baseline: float | None,
    max_count: float | None,
) -> float | None:
    if count is None or baseline is None or max_count is None or max_count <= 0:
        return None
    busyness = ((count - baseline) / max_count) * 100
    return max(0.0, min(100.0, busyness))


def _build_location_status(db: sqlite3.Connection) -> list[LocationStatus]:
    now = datetime.now(TIMEZONE)
    midnight_today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    lookback_start = now - timedelta(days=LOOKBACK_DAYS)

    # Fetch all smoothed counts within lookback window
    rows = db.execute(
        """
        SELECT location, timestamp, smoothed_count
        FROM smoothed_counts
        WHERE timestamp >= ?
        ORDER BY location, timestamp
        """,
        (lookback_start.isoformat(sep=" ", timespec="seconds"),),
    ).fetchall()

    if not rows:
        raise HTTPException(status_code=503, detail="No data available")

    counts = [
        SmoothedCount(
            location=row["location"],
            timestamp=datetime.fromisoformat(row["timestamp"]),
            count=row["smoothed_count"],
        )
        for row in rows
    ]

    aggregates = _compute_aggregates(counts)

    # Group counts by location for building response
    by_location: dict[str, list[SmoothedCount]] = {}
    for c in counts:
        by_location.setdefault(c.location, []).append(c)

    results: list[LocationStatus] = []

    for location, location_counts in sorted(by_location.items()):
        agg = aggregates.get(location)
        if not agg:
            continue

        # Get latest count and past count for trend
        latest = location_counts[-1]
        past_count = (
            location_counts[-1 - TREND_LOOKBACK_ROWS].count
            if len(location_counts) > TREND_LOOKBACK_ROWS
            else None
        )

        # Calculate busyness
        busyness = _calculate_busyness(latest.count, agg.baseline, agg.max_count)

        # Calculate vs typical
        day_of_week = (latest.timestamp.weekday() + 1) % 7
        minutes = (
            latest.timestamp.hour * 60
            + (latest.timestamp.minute // TIME_BUCKET_SIZE) * TIME_BUCKET_SIZE
        )
        typical = agg.time_averages.get((day_of_week, minutes))

        vs_typical = None
        if typical and typical > 0:
            vs_typical = ((latest.count - typical) / typical) * 100

        # Calculate trend
        trend: Literal["Increasing", "Steady", "Decreasing"] | None = None
        if past_count and past_count > 0:
            change = (latest.count - past_count) / past_count
            if change > TREND_THRESHOLD:
                trend = "Increasing"
            elif change < -TREND_THRESHOLD:
                trend = "Decreasing"
            else:
                trend = "Steady"

        # Build today's data points
        today_data = [
            DataPoint(
                timestamp=c.timestamp,
                busyness_percentage=_calculate_busyness(
                    c.count, agg.baseline, agg.max_count
                ),
            )
            for c in location_counts
            if c.timestamp >= midnight_today
        ]

        results.append(
            LocationStatus(
                location=location,
                timestamp=latest.timestamp,
                busyness_percentage=busyness,
                vs_typical_percentage=vs_typical,
                trend=trend,
                today_data=today_data,
            )
        )

    return results


_cache: tuple[float, list[LocationStatus]] | None = None


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    logger.info(f"API starting, database at {DATABASE_PATH}")
    yield
    logger.info("API shutting down")


app = FastAPI(lifespan=lifespan, root_path="/api")
app.add_middleware(GZipMiddleware)


def get_db() -> Generator[sqlite3.Connection]:
    db = sqlite3.connect(DATABASE_PATH, timeout=5.0)
    db.row_factory = sqlite3.Row
    try:
        yield db
    finally:
        db.close()


@app.get("/health")
def health() -> str:
    return "Ok"


@app.get("/current")
def get_current(
    db: Annotated[sqlite3.Connection, Depends(get_db)],
) -> list[LocationStatus]:
    global _cache

    now = time()

    if _cache is not None:
        cached_time, cached_data = _cache
        if now - cached_time < CACHE_TTL:
            return cached_data

    results = _build_location_status(db)
    _cache = (now, results)

    return results
