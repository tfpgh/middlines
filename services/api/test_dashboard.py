import unittest
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, datetime, timedelta
from unittest.mock import patch

from dashboard import (
    MINUTE,
    TIMEZONE,
    Calibration,
    Dashboard,
    MinuteCount,
    SmoothedCount,
    build_location,
    busyness,
    calibrate,
    eligible_end,
    smooth_counts,
    trend_for,
    typical_slot,
)
from fastapi.testclient import TestClient

NOW = datetime(2026, 3, 4, 17, 10, 40, tzinfo=UTC)


def reading(bucket: datetime, count: int, node: str = "ross") -> MinuteCount:
    return MinuteCount(node, bucket, count, bucket + timedelta(seconds=50))


class FakeStore:
    def __init__(self, rows: Sequence[MinuteCount] = ()) -> None:
        self.rows = list(rows)
        self.calls: list[tuple[datetime, datetime]] = []
        self.fail = False
        self.fail_on_call: int | None = None

    def read_minute_counts(
        self, nodes: Sequence[str], start: datetime, end: datetime
    ) -> list[MinuteCount]:
        self.calls.append((start, end))
        if self.fail or len(self.calls) == self.fail_on_call:
            raise RuntimeError("Database unavailable")
        return sorted(
            (
                row
                for row in self.rows
                if row.node in nodes and start <= row.bucket < end
            ),
            key=lambda row: (row.node, row.bucket),
        )


class StatisticsTests(unittest.TestCase):
    def test_ema_and_gap_reset(self) -> None:
        start = NOW.replace(second=0)
        rows = [
            reading(start, 100),
            reading(start + MINUTE, 200),
            reading(start + MINUTE * 3, 50),
        ]
        series = smooth_counts(rows)["ross"]
        self.assertEqual([p.count for p in series], [100, 136, 50])
        self.assertAlmostEqual(0.64**60, 0.8**120)

    def test_five_minute_trend_and_boundaries(self) -> None:
        for count, expected in [
            (108, "Increasing"),
            (107, "Steady"),
            (93, "Steady"),
            (92, "Decreasing"),
        ]:
            with self.subTest(count=count):
                span = [
                    SmoothedCount(NOW + MINUTE * i, 100 if i < 5 else count, NOW)
                    for i in range(6)
                ]
                self.assertEqual(trend_for(span, 50), expected)
                self.assertIsNone(trend_for(span, 9))
                self.assertIsNone(trend_for(span[:-1], 50))
                gap = [
                    SmoothedCount(
                        p.bucket + (MINUTE if i == 5 else timedelta()), p.count, NOW
                    )
                    for i, p in enumerate(span)
                ]
                self.assertIsNone(trend_for(gap, 50))
        zero = [SmoothedCount(NOW + MINUTE * i, 0, NOW) for i in range(6)]
        self.assertIsNone(trend_for(zero, 50))

    def test_calibration_baseline_percentile_and_typical(self) -> None:
        overnight = datetime(2026, 3, 4, 7, tzinfo=UTC)  # 02:00 Eastern
        slot = datetime(2026, 3, 3, 17, tzinfo=UTC)
        series = [
            SmoothedCount(NOW - timedelta(days=46), 10000, NOW),
            SmoothedCount(slot, 100, slot),
            SmoothedCount(overnight, 10, overnight),
            SmoothedCount(overnight + MINUTE, 30, overnight + MINUTE),
            SmoothedCount(slot + timedelta(days=1), 200, NOW),
        ]
        calibration = calibrate(series, NOW)
        self.assertEqual(calibration.baseline, 20)
        self.assertEqual(calibration.scale, 180)
        self.assertEqual(calibration.typical[typical_slot(slot)], 150)
        percentage = busyness(110, calibration)
        assert percentage is not None
        self.assertAlmostEqual(percentage, 50)
        self.assertEqual(busyness(0, calibration), 0)
        self.assertEqual(busyness(1000, calibration), 100)
        self.assertIsNone(busyness(10, Calibration(10, 0, {})))

    def test_exact_percentile_index(self) -> None:
        start = NOW - timedelta(days=3)
        # Constant overnight baseline, with 2,001 qualifying daytime samples.
        points = [SmoothedCount(start, float(i), start) for i in range(1, 2002)]
        self.assertEqual(calibrate(points, NOW).scale, 2000)

    def test_eligibility_and_eastern_slots(self) -> None:
        self.assertEqual(eligible_end(NOW), NOW.replace(second=0))
        self.assertEqual(
            eligible_end(NOW.replace(second=20)), NOW.replace(second=0) - MINUTE
        )
        self.assertEqual(
            typical_slot(datetime(2026, 3, 7, 3, tzinfo=UTC)), (False, 22 * 60)
        )
        self.assertEqual(typical_slot(datetime(2026, 3, 7, 6, tzinfo=UTC)), (True, 60))
        # Repeated fall-back hour is one typical local slot, two different instants.
        self.assertEqual(
            typical_slot(datetime(2026, 11, 1, 5, 30, tzinfo=UTC)),
            typical_slot(datetime(2026, 11, 1, 6, 30, tzinfo=UTC)),
        )

    def test_statuses_and_chart_gaps(self) -> None:
        end = eligible_end(NOW)
        midnight = end - MINUTE * 3
        series = [
            SmoothedCount(end - MINUTE * 3, 100, end - MINUTE * 2),
            SmoothedCount(end - MINUTE, 10, end),
        ]
        calibration = Calibration(10, 100, {})
        location = build_location("Ross", series, calibration, NOW, midnight, end)
        self.assertEqual(location.status, "closed")
        self.assertEqual(
            [p.busyness_percentage for p in location.today_data], [90, None, None]
        )
        self.assertEqual(location.today_data[0].timestamp, midnight + MINUTE)
        self.assertEqual(
            build_location(
                "Ross", series, calibration, NOW + MINUTE * 6, midnight, end
            ).status,
            "stale",
        )
        self.assertEqual(
            build_location("Ross", series[:-1], calibration, NOW, midnight, end).status,
            "unavailable",
        )
        self.assertEqual(
            build_location("Ross", [], calibration, NOW, midnight, end).status,
            "unavailable",
        )

    def test_dst_chart_sizes_and_continuous_utc_timestamps(self) -> None:
        for month, day, length in [(3, 8, 1380), (11, 1, 1500)]:
            with self.subTest(month=month):
                local = datetime(2026, month, day, tzinfo=TIMEZONE)
                start = local.astimezone(UTC)
                end = (local + timedelta(days=1)).astimezone(UTC)
                result = build_location(
                    "Ross", [], Calibration(0, 0, {}), end, start, end
                )
                self.assertEqual(len(result.today_data), length)
                self.assertEqual(
                    len({point.timestamp for point in result.today_data}), length
                )


class DashboardTests(unittest.TestCase):
    def setUp(self) -> None:
        self.now = NOW
        self.elapsed = 0.0
        end = eligible_end(self.now)
        self.store = FakeStore(
            [reading(end - MINUTE * i, 100 + (6 - i) * 5) for i in range(6, 0, -1)]
        )
        self.dashboard = Dashboard(
            self.store, clock=lambda: self.now, timer=lambda: self.elapsed
        )

    def test_locations_cache_lifetimes_and_bounds(self) -> None:
        response = self.dashboard.current()
        self.assertEqual(
            [hall.location for hall in response], ["Atwater", "Proctor", "Ross", "Test"]
        )
        self.assertEqual(response[2].status, "active")
        self.assertEqual(response[0].status, "unavailable")
        self.assertIsNone(response[0].timestamp)
        self.assertIs(self.dashboard.current(), response)
        self.assertEqual(len(self.store.calls), 2)
        self.assertLessEqual(
            (self.store.calls[-1][1] - self.store.calls[-1][0]).total_seconds(),
            26 * 3600,
        )
        self.now += MINUTE
        self.elapsed += 60
        self.dashboard.current()
        self.assertEqual(len(self.store.calls), 3)
        self.now += MINUTE * 15
        self.elapsed += 900
        self.dashboard.current()
        self.assertEqual(len(self.store.calls), 5)

    def test_concurrent_requests_share_one_rebuild(self) -> None:
        with ThreadPoolExecutor(max_workers=12) as pool:
            futures = [pool.submit(self.dashboard.current) for _ in range(24)]
            results = [future.result() for future in futures]
        self.assertEqual(len(self.store.calls), 2)
        self.assertTrue(all(result is results[0] for result in results))

    def test_test_node_readings_reach_dashboard(self) -> None:
        end = eligible_end(self.now)
        self.store.rows.extend(
            reading(end - MINUTE * i, 100 + (6 - i) * 5, node="test")
            for i in range(6, 0, -1)
        )
        location = next(
            item for item in self.dashboard.current() if item.location == "Test"
        )
        self.assertEqual(location.status, "active")
        self.assertEqual(location.timestamp, end - timedelta(seconds=10))
        self.assertIsNotNone(location.busyness_percentage)
        self.assertEqual(location.trend, "Increasing")
        self.assertEqual(
            location.today_data[-1].busyness_percentage, location.busyness_percentage
        )

    def test_failed_rebuild_is_retried(self) -> None:
        self.store.fail = True
        with self.assertRaises(RuntimeError):
            self.dashboard.current()
        self.store.fail = False
        self.assertEqual(self.dashboard.current()[2].status, "active")
        self.assertEqual(len(self.store.calls), 3)

    def test_partial_rebuild_does_not_publish_calibration(self) -> None:
        self.store.fail_on_call = 2
        with self.assertRaises(RuntimeError):
            self.dashboard.current()
        self.dashboard.current()
        self.assertEqual(len(self.store.calls), 4)

    def test_midnight_invalidates_same_eligible_minute(self) -> None:
        self.now = datetime(2026, 3, 5, 4, 59, 50, tzinfo=UTC)
        self.dashboard.current()
        self.now += timedelta(seconds=20)
        result = self.dashboard.current()
        self.assertEqual(len(self.store.calls), 3)
        self.assertTrue(all(hall.today_data == [] for hall in result))

    def test_http_contract_and_database_failure(self) -> None:
        import main

        with patch.object(main, "_dashboard", self.dashboard):
            client = TestClient(main.app)
            response = client.get("/current")
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.headers["cache-control"], "no-store")
            self.assertEqual(
                [item["location"] for item in response.json()],
                ["Atwater", "Proctor", "Ross", "Test"],
            )
            self.assertNotIn("mac", response.text)
            self.assertNotIn("devices", response.text)
            self.now += MINUTE
            self.store.fail = True
            with patch.object(main.logger, "exception"):
                self.assertEqual(client.get("/current").status_code, 503)


if __name__ == "__main__":
    unittest.main()
