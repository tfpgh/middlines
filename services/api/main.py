import json
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

# Trend calculation - compare current to N rows back
TREND_LOOKBACK_ROWS = 5

# Trend threshold - decimal change to determine increasing/decreasing (0.07 = 7%)
TREND_THRESHOLD = 0.07

TIME_BUCKET_SIZE = int(os.environ.get("TIME_BUCKET_SIZE", "10"))

# Seconds
CACHE_TTL = 30


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


# Simple cache: (timestamp, data) or None
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


def _calculate_busyness(
    count: float | None,
    baseline: float | None,
    adjusted_max: float | None,
) -> float | None:
    if count is None or baseline is None or adjusted_max is None or adjusted_max <= 0:
        return None
    busyness = ((count - baseline) / adjusted_max) * 100
    return max(0.0, min(100.0, busyness))


def _build_location_status(
    db: sqlite3.Connection,
) -> list[LocationStatus]:
    now = datetime.now(TIMEZONE)
    midnight_today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    yesterday = now - timedelta(days=1)

    rows = db.execute(
        """
                WITH
                -- 1. Limit to recent data, then rank by location (most recent first)
                recent_counts AS (
                    SELECT
                        location,
                        timestamp,
                        smoothed_count
                    FROM
                        smoothed_counts
                    WHERE
                        timestamp >= ?
                ),
                ranked_counts AS (
                    SELECT
                        location,
                        timestamp,
                        smoothed_count,
                        ROW_NUMBER() OVER (PARTITION BY location ORDER BY timestamp DESC) AS rn
                    FROM
                        recent_counts
                ),
                -- 2. Isolate the latest data and the specific past data point for trend analysis.
                latest_trend_data AS (
                    SELECT
                        rc1.location,
                        rc1.timestamp,
                        rc1.smoothed_count,
                        rc2.smoothed_count AS past_count
                    FROM
                        ranked_counts rc1
                    LEFT JOIN
                        ranked_counts rc2 ON rc1.location = rc2.location AND rc2.rn = rc1.rn + ?
                    WHERE
                        rc1.rn = 1
                ),
                -- 3. Aggregate today's data into a single JSON object per location.
                today_agg AS (
                    SELECT
                        location,
                        json_group_array(json_object('timestamp', timestamp, 'smoothed_count', smoothed_count)) AS today_json
                    FROM
                        recent_counts
                    WHERE
                        timestamp >= ?
                    GROUP BY
                        location
                )
                -- 4. Main query to join everything together.
                SELECT
                    ltd.location,
                    ltd.timestamp,
                    ltd.smoothed_count,
                    ltd.past_count,
                    b.baseline_count,
                    mc.baseline_adjusted_max_count,
                    ta.mean_count,
                    tda.today_json
                FROM
                    latest_trend_data ltd
                LEFT JOIN
                    baseline b ON ltd.location = b.location
                LEFT JOIN
                    max_count mc ON ltd.location = mc.location
                LEFT JOIN
                    time_averages ta ON ltd.location = ta.location
                    AND ta.day_of_week = CAST(strftime('%w', ltd.timestamp) AS INTEGER)
                    AND ta.time_bucket = (CAST(strftime('%H', ltd.timestamp) AS INTEGER) * 60) + (CAST(strftime('%M', ltd.timestamp) AS INTEGER) / ?) * ?
                LEFT JOIN
                    today_agg tda ON ltd.location = tda.location
                ORDER BY
                    ltd.location;
                """,
        (
            yesterday.isoformat(sep=" ", timespec="seconds"),
            TREND_LOOKBACK_ROWS,
            midnight_today.isoformat(sep=" ", timespec="seconds"),
            TIME_BUCKET_SIZE,
            TIME_BUCKET_SIZE,
        ),
    ).fetchall()

    if not rows:
        raise HTTPException(status_code=503, detail="No data available")

    results: list[LocationStatus] = []
    for row in rows:
        current_count = row["smoothed_count"]
        baseline = row["baseline_count"]
        adjusted_max = row["baseline_adjusted_max_count"]
        typical_count = row["mean_count"]
        past_count = row["past_count"]

        busyness_percentage = _calculate_busyness(current_count, baseline, adjusted_max)

        vs_typical_percentage = None
        if typical_count and typical_count > 0 and current_count is not None:
            vs_typical_percentage = (
                (current_count - typical_count) / typical_count
            ) * 100

        trend = None
        if past_count and past_count > 0 and current_count is not None:
            change_ratio = (current_count - past_count) / past_count
            if change_ratio > TREND_THRESHOLD:
                trend = "Increasing"
            elif change_ratio < -TREND_THRESHOLD:
                trend = "Decreasing"
            else:
                trend = "Steady"

        today_data = []
        if row["today_json"]:
            today_points = json.loads(row["today_json"])
            for point in today_points:
                busyness = _calculate_busyness(
                    point["smoothed_count"], baseline, adjusted_max
                )
                today_data.append(
                    DataPoint(
                        timestamp=datetime.fromisoformat(point["timestamp"]),
                        busyness_percentage=busyness,
                    )
                )

        results.append(
            LocationStatus(
                location=row["location"],
                timestamp=datetime.fromisoformat(row["timestamp"]),
                busyness_percentage=busyness_percentage,
                vs_typical_percentage=vs_typical_percentage,
                trend=trend,
                today_data=sorted(today_data, key=lambda p: p.timestamp),
            )
        )

    return results


@app.get("/current")
def get_current(
    db: Annotated[sqlite3.Connection, Depends(get_db)],
) -> list[LocationStatus]:
    global _cache

    now = time()

    # Return cached data if valid
    if _cache is not None:
        cached_time, cached_data = _cache
        if now - cached_time < CACHE_TTL:
            return cached_data

    # Build fresh data
    results = _build_location_status(db)

    # Update cache
    _cache = (now, results)

    return results
