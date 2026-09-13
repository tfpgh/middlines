import os
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Self, cast
from uuid import UUID

import clickhouse_connect
from clickhouse_connect.driver.client import Client
from dashboard import MinuteCount

_FIXED_STRING_FORMAT = {"FixedString": "string"}


@dataclass(frozen=True, slots=True)
class NodeConfig:
    node: str
    token: str
    poll_interval_s: int
    target_firmware_sha256: str | None
    restart_nonce: UUID | None


@dataclass(frozen=True, slots=True)
class FirmwareArtifact:
    sha256: str
    version: str
    filename: str
    original_filename: str
    size_bytes: int
    uploaded_at: datetime


@dataclass(frozen=True, slots=True)
class NodeCheckin:
    node: str
    firmware_version: str
    client_ip: str
    seen_at: datetime


class ClickHouseStore:
    def __init__(self, client: Client) -> None:
        self._client = client

    @classmethod
    def from_env(cls) -> Self:
        return cls.connect(
            host=os.environ.get("MIDDLINES_CLICKHOUSE_HOST", "localhost"),
            port=int(os.environ.get("MIDDLINES_CLICKHOUSE_PORT", "8123")),
            database=os.environ.get("MIDDLINES_CLICKHOUSE_DATABASE", "middlines"),
            username=os.environ.get("MIDDLINES_CLICKHOUSE_USERNAME", "default"),
            password=os.environ.get("MIDDLINES_CLICKHOUSE_PASSWORD", ""),
        )

    @classmethod
    def connect(
        cls,
        *,
        host: str,
        port: int,
        database: str,
        username: str,
        password: str,
    ) -> Self:
        return cls(
            clickhouse_connect.get_client(  # pyright: ignore[reportUnknownMemberType]
                host=host,
                port=port,
                database=database,
                username=username,
                password=password,
                tz_mode="schema",
                autogenerate_session_id=False,
                connect_timeout=5,
                send_receive_timeout=10,
            )
        )

    def close(self) -> None:
        self._client.close()

    def read_minute_counts(
        self, nodes: Sequence[str], start: datetime, end: datetime
    ) -> list[MinuteCount]:
        # Exact states are much larger than scalar counts. Bound both read blocks
        # and each ordered-aggregation buffer, including with unmerged parts.
        rows = self._query_rows(
            """
            SELECT node, bucket, uniqExactMerge(devices), max(last_observed_at)
            FROM device_counts_1m
            WHERE node IN {nodes:Array(String)}
              AND bucket >= {start:DateTime('UTC')}
              AND bucket < {end:DateTime('UTC')}
            GROUP BY node, bucket
            ORDER BY node, bucket
            SETTINGS optimize_aggregation_in_order = 1,
                     aggregation_in_order_max_block_bytes = 262144,
                     max_block_size = 128,
                     max_memory_usage = 536870912, max_execution_time = 8
            """,
            {"nodes": list(nodes), "start": start, "end": end},
        )
        return [
            MinuteCount(
                cast(str, row[0]),
                cast(datetime, row[1]).astimezone(UTC),
                cast(int, row[2]),
                cast(datetime, row[3]).astimezone(UTC),
            )
            for row in rows
        ]

    def backfill_minute_counts(self, start: datetime, end: datetime) -> None:
        # Set union and maximum make retries and overlap with the live view safe.
        self._client.command(  # pyright: ignore[reportUnknownMemberType]
            """
            INSERT INTO device_counts_1m
            SELECT node,
                   toDateTime(toStartOfMinute(observed_at), 'UTC') AS bucket,
                   uniqExactState(mac), max(observed_at)
            FROM observations_raw
            WHERE observed_at >= {start:DateTime64(3, 'UTC')}
              AND observed_at < {end:DateTime64(3, 'UTC')}
              AND rssi >= -120
            GROUP BY node, bucket
            SETTINGS max_threads = 1, max_memory_usage = 536870912
            """,
            parameters={"start": start, "end": end},
        )

    def ensure_nodes(self, nodes: Iterable[str], default_poll_interval_s: int) -> None:
        existing = {config.node for config in self.list_latest_node_configs()}
        missing = [
            NodeConfig(
                node=node,
                token="",
                poll_interval_s=default_poll_interval_s,
                target_firmware_sha256=None,
                restart_nonce=None,
            )
            for node in nodes
            if node not in existing
        ]
        if missing:
            self._insert_node_configs(missing)

    def list_latest_node_configs(self) -> list[NodeConfig]:
        rows = self._query_rows(
            """
            SELECT node, token, poll_interval_s,
                   target_firmware_sha256, restart_nonce
            FROM node_config_history
            ORDER BY node, changed_at DESC
            LIMIT 1 BY node
            """
        )
        return [self._node_config_from_row(row) for row in rows]

    def get_latest_node_config(self, node: str) -> NodeConfig | None:
        rows = self._query_rows(
            """
            SELECT node, token, poll_interval_s,
                   target_firmware_sha256, restart_nonce
            FROM node_config_history
            WHERE node = {node:String}
            ORDER BY changed_at DESC
            LIMIT 1
            """,
            {"node": node},
        )
        return self._node_config_from_row(rows[0]) if rows else None

    def append_node_config(self, config: NodeConfig) -> None:
        self._insert_node_configs([config])

    def _insert_node_configs(self, configs: Sequence[NodeConfig]) -> None:
        self._client.insert(
            "node_config_history",
            [
                [
                    config.node,
                    config.token,
                    config.poll_interval_s,
                    config.target_firmware_sha256,
                    config.restart_nonce,
                ]
                for config in configs
            ],
            column_names=[
                "node",
                "token",
                "poll_interval_s",
                "target_firmware_sha256",
                "restart_nonce",
            ],
        )

    def list_firmware_artifacts(self) -> list[FirmwareArtifact]:
        rows = self._query_rows(
            """
            SELECT sha256, version, filename, original_filename, size_bytes, uploaded_at
            FROM firmware_artifacts
            ORDER BY uploaded_at DESC, sha256
            """
        )
        return [self._firmware_artifact_from_row(row) for row in rows]

    def get_firmware_artifact(self, sha256: str) -> FirmwareArtifact | None:
        rows = self._query_rows(
            """
            SELECT sha256, version, filename, original_filename, size_bytes, uploaded_at
            FROM firmware_artifacts
            WHERE sha256 = toFixedString({sha256:String}, 64)
            ORDER BY uploaded_at DESC
            LIMIT 1
            """,
            {"sha256": sha256},
        )
        return self._firmware_artifact_from_row(rows[0]) if rows else None

    def add_firmware_artifact(
        self,
        *,
        sha256: str,
        version: str,
        filename: str,
        original_filename: str,
        size_bytes: int,
    ) -> None:
        self._client.insert(
            "firmware_artifacts",
            [[sha256, version, filename, original_filename, size_bytes]],
            column_names=[
                "sha256",
                "version",
                "filename",
                "original_filename",
                "size_bytes",
            ],
        )

    def list_latest_node_checkins(self) -> list[NodeCheckin]:
        rows = self._query_rows(
            """
            SELECT node, firmware_version, client_ip, seen_at
            FROM node_checkins
            ORDER BY node, seen_at DESC
            LIMIT 1 BY node
            """
        )
        return [self._node_checkin_from_row(row) for row in rows]

    def record_node_checkin(
        self,
        *,
        node: str,
        firmware_version: str,
        client_ip: str,
    ) -> None:
        self._client.insert(
            "node_checkins",
            [[node, firmware_version, client_ip]],
            column_names=["node", "firmware_version", "client_ip"],
        )

    def insert_observations(
        self,
        node: str,
        observations: Sequence[tuple[int, int, int]],
    ) -> None:
        self._client.insert(
            "observations_raw",
            [
                [observed_at_ms, node, mac, rssi]
                for observed_at_ms, mac, rssi in observations
            ],
            column_names=["observed_at", "node", "mac", "rssi"],
            settings={"async_insert": 1, "wait_for_async_insert": 0},
        )

    def _query_rows(
        self,
        query: str,
        parameters: dict[str, object] | None = None,
    ) -> list[Sequence[object]]:
        result = self._client.query(  # pyright: ignore[reportUnknownMemberType]
            query,
            parameters=parameters,
            query_formats=_FIXED_STRING_FORMAT,
        )
        return cast(list[Sequence[object]], result.result_rows)

    @staticmethod
    def _node_config_from_row(row: Sequence[object]) -> NodeConfig:
        return NodeConfig(
            node=cast(str, row[0]),
            token=cast(str, row[1]),
            poll_interval_s=cast(int, row[2]),
            target_firmware_sha256=cast(str | None, row[3]),
            restart_nonce=cast(UUID | None, row[4]),
        )

    @staticmethod
    def _firmware_artifact_from_row(row: Sequence[object]) -> FirmwareArtifact:
        return FirmwareArtifact(
            sha256=cast(str, row[0]),
            version=cast(str, row[1]),
            filename=cast(str, row[2]),
            original_filename=cast(str, row[3]),
            size_bytes=cast(int, row[4]),
            uploaded_at=cast(datetime, row[5]),
        )

    @staticmethod
    def _node_checkin_from_row(row: Sequence[object]) -> NodeCheckin:
        return NodeCheckin(
            node=cast(str, row[0]),
            firmware_version=cast(str, row[1]),
            client_ip=cast(str, row[2]),
            seen_at=cast(datetime, row[3]),
        )
