"""Integration tests: set MIDDLINES_TEST_CLICKHOUSE_PORT for an isolated server.

Each run creates and drops its own randomly named database. No production database
or MIDDLINES_CLICKHOUSE_* connection settings are used.
"""

import os
import unittest
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import cast
from uuid import uuid4

import clickhouse_connect
from clickhouse_connect.driver.client import Client
from store import ClickHouseStore

MIGRATIONS = Path(__file__).resolve().parents[2] / "clickhouse" / "migrations"


@unittest.skipUnless(
    os.environ.get("MIDDLINES_TEST_CLICKHOUSE_PORT"),
    "Requires an isolated ClickHouse test server",
)
class ClickHouseTests(unittest.TestCase):
    client: Client
    store: ClickHouseStore
    database: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.database = "middlines_dashboard_test_" + uuid4().hex
        port = int(os.environ["MIDDLINES_TEST_CLICKHOUSE_PORT"])
        cls.client = clickhouse_connect.get_client(  # pyright: ignore[reportUnknownMemberType]
            host="127.0.0.1", port=port, autogenerate_session_id=False, tz_mode="schema"
        )
        cls.addClassCleanup(cls.client.close)
        cls.client.command("CREATE DATABASE " + cls.database)  # pyright: ignore[reportUnknownMemberType]
        cls.addClassCleanup(lambda: cls.client.command("DROP DATABASE " + cls.database))  # pyright: ignore[reportUnknownMemberType]
        for filename in [
            "00002_create_observations.sql",
            "00006_create_device_counts_1m.sql",
        ]:
            sql = (MIGRATIONS / filename).read_text().split("-- +goose Down")[0]
            for statement in sql.replace("middlines.", cls.database + ".").split(";"):
                if statement.strip():
                    cls.client.command(statement)  # pyright: ignore[reportUnknownMemberType]
        cls.store = ClickHouseStore.connect(
            host="127.0.0.1",
            port=port,
            database=cls.database,
            username="default",
            password="",
        )
        cls.addClassCleanup(cls.store.close)

    def setUp(self) -> None:
        for table in ["observations_raw", "device_counts_1m"]:
            self.client.command(f"TRUNCATE TABLE {self.database}.{table}")  # pyright: ignore[reportUnknownMemberType]

    def insert(self, rows: list[list[object]]) -> None:
        self.client.insert(
            self.database + ".observations_raw",
            rows,
            column_names=["observed_at", "node", "mac", "rssi"],
        )

    def test_live_view_duplicates_late_data_and_backfill(self) -> None:
        start = datetime(2026, 3, 4, 17, tzinfo=UTC)
        millis = int(start.timestamp() * 1000)
        self.insert(
            [
                [millis, "ross", 1, -70],
                [millis + 1000, "ross", 1, -70],
                [millis + 60000, "ross", 1, -70],
            ]
        )
        self.insert(
            [
                [millis + 500, "ross", 1, -70],
                [millis + 2000, "ross", 2, -120],
                [millis + 3000, "ross", 3, -121],
                [millis, "proctor", 1, -70],
            ]
        )
        end = start + timedelta(minutes=2)
        before = self.store.read_minute_counts(["ross"], start, end)
        self.assertEqual([row.count for row in before], [2, 1])
        self.assertEqual(before[0].last_observed_at, start + timedelta(seconds=2))
        self.store.backfill_minute_counts(start, end)
        self.store.backfill_minute_counts(start, end)
        self.assertEqual(self.store.read_minute_counts(["ross"], start, end), before)
        self.insert([[millis + 4000, "ross", 4, -70]])
        after = self.store.read_minute_counts(["ross"], start, end)
        self.assertEqual([row.count for row in after], [3, 1])
        self.assertEqual(
            len(
                self.store.read_minute_counts(
                    ["ross"], start, start + timedelta(minutes=1)
                )
            ),
            1,
        )
        result = self.client.query(  # pyright: ignore[reportUnknownMemberType]
            f"SELECT uniqExact(mac) FROM {self.database}.observations_raw WHERE node='ross' AND rssi >= -120 AND observed_at < toDateTime64('2026-03-04 17:01:00', 3, 'UTC')"
        )
        self.assertEqual(after[0].count, cast(int, result.result_rows[0][0]))

    def test_dst_repeated_hour_keeps_distinct_buckets(self) -> None:
        first = datetime(2026, 11, 1, 5, 30, tzinfo=UTC)
        second = first + timedelta(hours=1)
        self.insert(
            [[int(t.timestamp() * 1000), "ross", 1, -70] for t in [first, second]]
        )
        rows = self.store.read_minute_counts(
            ["ross"], first, second + timedelta(minutes=1)
        )
        self.assertEqual([row.bucket for row in rows], [first, second])

    def test_history_from_before_view_creation_and_migration_down(self) -> None:
        migration = (MIGRATIONS / "00006_create_device_counts_1m.sql").read_text()
        up, down = migration.replace("middlines.", self.database + ".").split(
            "-- +goose Down"
        )
        for statement in down.split(";"):
            if statement.strip():
                self.client.command(statement)  # pyright: ignore[reportUnknownMemberType]
        start = datetime(2026, 3, 4, 17, tzinfo=UTC)
        self.insert([[int(start.timestamp() * 1000), "ross", 42, -70]])
        for statement in up.split(";"):
            if statement.strip():
                self.client.command(statement)  # pyright: ignore[reportUnknownMemberType]
        end = start + timedelta(minutes=1)
        self.assertEqual(self.store.read_minute_counts(["ross"], start, end), [])
        self.store.backfill_minute_counts(start, end)
        self.assertEqual(
            self.store.read_minute_counts(["ross"], start, end)[0].count, 1
        )


if __name__ == "__main__":
    unittest.main()
