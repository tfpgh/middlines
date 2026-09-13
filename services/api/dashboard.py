from collections.abc import Callable, Sequence
from dataclasses import dataclass
from datetime import UTC, date, datetime, timedelta
from threading import Lock
from time import monotonic
from typing import Literal, Protocol
from zoneinfo import ZoneInfo

from pydantic import BaseModel

TIMEZONE = ZoneInfo("America/New_York")
LOCATIONS = {"atwater": "Atwater", "proctor": "Proctor", "ross": "Ross", "test": "Test"}
MINUTE = timedelta(minutes=1)
WARMUP = timedelta(hours=1)
LOOKBACK = timedelta(days=45)
INGESTION_GRACE = timedelta(seconds=30)
STALE_AFTER = timedelta(minutes=5)
EMA_ALPHA = 0.36
CALIBRATION_TTL = 15 * 60
MAX_PERCENTILE = 0.9995
CLOSED_THRESHOLD = 1.5
TREND_MIN_BUSYNESS = 10
TREND_THRESHOLD = 0.07
TREND_MINUTES = 5

type Trend = Literal["Increasing", "Steady", "Decreasing"]
type Status = Literal["active", "closed", "stale", "unavailable"]
type TypicalSlot = tuple[bool, int]


class DataPoint(BaseModel):
    timestamp: datetime
    busyness_percentage: float | None


class LocationStatus(BaseModel):
    location: str
    timestamp: datetime | None
    status: Status
    busyness_percentage: float | None
    vs_typical_percentage: float | None
    trend: Trend | None
    today_data: list[DataPoint]


@dataclass(frozen=True, slots=True)
class MinuteCount:
    node: str
    bucket: datetime
    count: int
    last_observed_at: datetime


@dataclass(frozen=True, slots=True)
class SmoothedCount:
    bucket: datetime
    count: float
    last_observed_at: datetime


class CountStore(Protocol):
    def read_minute_counts(
        self, nodes: Sequence[str], start: datetime, end: datetime
    ) -> list[MinuteCount]: ...


@dataclass(frozen=True, slots=True)
class Calibration:
    baseline: float
    scale: float
    typical: dict[TypicalSlot, float]


def eligible_end(now: datetime) -> datetime:
    """Exclusive end of complete buckets, allowing async inserts time to flush."""
    return (now.astimezone(UTC) - INGESTION_GRACE).replace(second=0, microsecond=0)


def typical_slot(bucket: datetime) -> TypicalSlot:
    local = bucket.astimezone(TIMEZONE)
    return local.weekday() >= 5, local.hour * 60 + local.minute


def smooth_counts(rows: Sequence[MinuteCount]) -> dict[str, list[SmoothedCount]]:
    """Rows are ordered by node and bucket by the store; gaps start a new EMA."""
    result: dict[str, list[SmoothedCount]] = {}
    for row in rows:
        series = result.setdefault(row.node, [])
        count = float(row.count)
        if series and row.bucket - series[-1].bucket == MINUTE:
            count = EMA_ALPHA * count + (1 - EMA_ALPHA) * series[-1].count
        series.append(SmoothedCount(row.bucket, count, row.last_observed_at))
    return result


def calibrate(series: Sequence[SmoothedCount], now: datetime) -> Calibration:
    cutoff = now - LOOKBACK
    yesterday = now - timedelta(days=1)
    overnight = [
        point.count
        for point in series
        if point.bucket > yesterday and 1 <= point.bucket.astimezone(TIMEZONE).hour < 4
    ]
    baseline = sum(overnight) / len(overnight) if overnight else 0.0
    active = [
        point
        for point in series
        if point.bucket > cutoff and point.count > baseline * CLOSED_THRESHOLD
    ]
    counts = sorted(point.count for point in active)
    scale = (
        counts[min(int(len(counts) * MAX_PERCENTILE), len(counts) - 1)] - baseline
        if counts
        else 0.0
    )
    slots: dict[TypicalSlot, tuple[float, int]] = {}
    for point in active:
        key = typical_slot(point.bucket)
        total, samples = slots.get(key, (0.0, 0))
        slots[key] = total + point.count, samples + 1
    return Calibration(
        baseline,
        scale,
        {key: total / samples for key, (total, samples) in slots.items()},
    )


def busyness(count: float, calibration: Calibration) -> float | None:
    if calibration.scale <= 0:
        return None
    return max(
        0.0, min(100.0, 100 * (count - calibration.baseline) / calibration.scale)
    )


def is_closed(count: float, calibration: Calibration) -> bool:
    return calibration.baseline > 0 and count <= calibration.baseline * CLOSED_THRESHOLD


def trend_for(series: Sequence[SmoothedCount], percentage: float) -> Trend | None:
    if percentage < TREND_MIN_BUSYNESS or len(series) < TREND_MINUTES + 1:
        return None
    span = series[-(TREND_MINUTES + 1) :]
    if span[-1].bucket - span[0].bucket != MINUTE * TREND_MINUTES or span[0].count <= 0:
        return None
    change = (span[-1].count - span[0].count) / span[0].count
    if change > TREND_THRESHOLD:
        return "Increasing"
    if change < -TREND_THRESHOLD:
        return "Decreasing"
    return "Steady"


def build_location(
    name: str,
    series: Sequence[SmoothedCount],
    calibration: Calibration,
    now: datetime,
    midnight: datetime,
    end: datetime,
) -> LocationStatus:
    latest = series[-1] if series else None
    status: Status = "unavailable"
    percentage = None
    versus = None
    trend = None
    if latest:
        if now - latest.last_observed_at > STALE_AFTER:
            status = "stale"
        elif latest.bucket == end - MINUTE and calibration.scale > 0:
            if is_closed(latest.count, calibration):
                status = "closed"
            else:
                status = "active"
                percentage = busyness(latest.count, calibration)
                typical = calibration.typical.get(typical_slot(latest.bucket))
                if typical is not None and typical > 0:
                    versus = 100 * (latest.count - typical) / typical
                if percentage is not None:
                    trend = trend_for(series, percentage)

    by_bucket = {point.bucket: point for point in series if point.bucket >= midnight}
    today: list[DataPoint] = []
    bucket = midnight
    while bucket < end:
        point = by_bucket.get(bucket)
        value = (
            busyness(point.count, calibration)
            if point and not is_closed(point.count, calibration)
            else None
        )
        today.append(DataPoint(timestamp=bucket + MINUTE, busyness_percentage=value))
        bucket += MINUTE

    return LocationStatus(
        location=name,
        timestamp=latest.last_observed_at if latest else None,
        status=status,
        busyness_percentage=percentage,
        vs_typical_percentage=versus,
        trend=trend,
        today_data=today,
    )


class Dashboard:
    """Two bounded, lazy caches shared by the API process; no background worker."""

    def __init__(
        self,
        store: CountStore,
        *,
        clock: Callable[[], datetime] = lambda: datetime.now(UTC),
        timer: Callable[[], float] = monotonic,
    ) -> None:
        self._store = store
        self._clock = clock
        self._timer = timer
        self._lock = Lock()
        self._calibration: dict[str, Calibration] = {}
        self._calibration_expires = 0.0
        self._response_key: tuple[datetime, date] | None = None
        self._response: list[LocationStatus] = []

    def current(self) -> list[LocationStatus]:
        with self._lock:
            now = self._clock().astimezone(UTC)
            end = eligible_end(now)
            local = now.astimezone(TIMEZONE)
            key = (end, local.date())
            if key == self._response_key:
                return self._response

            nodes = tuple(LOCATIONS)
            if self._timer() >= self._calibration_expires or not self._calibration:
                history = smooth_counts(
                    self._store.read_minute_counts(
                        nodes,
                        (now - LOOKBACK - WARMUP).replace(second=0, microsecond=0),
                        end,
                    )
                )
                calibration = {
                    node: calibrate(history.get(node, []), now) for node in nodes
                }
            else:
                calibration = self._calibration

            midnight = local.replace(
                hour=0, minute=0, second=0, microsecond=0
            ).astimezone(UTC)
            today = smooth_counts(
                self._store.read_minute_counts(nodes, midnight - WARMUP, end)
            )
            response = [
                build_location(
                    name, today.get(node, []), calibration[node], now, midnight, end
                )
                for node, name in LOCATIONS.items()
            ]
            # Publish only after both queries and the complete response succeed.
            if calibration is not self._calibration:
                self._calibration = calibration
                self._calibration_expires = self._timer() + CALIBRATION_TTL
            self._response = response
            self._response_key = key
            return response
