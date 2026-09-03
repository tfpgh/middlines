import unittest
from unittest.mock import Mock

from clickhouse_connect.driver.client import Client
from store import ClickHouseStore


class InsertObservationsTests(unittest.TestCase):
    def test_submits_one_async_insert_without_waiting(self) -> None:
        client = Mock(spec=Client)
        store = ClickHouseStore(client)

        store.insert_observations("ross", [(1_700_000_000_123, 0xAABBCCDDEEFF, -72)])

        client.insert.assert_called_once_with(
            "observations_raw",
            [[1_700_000_000_123, "ross", 0xAABBCCDDEEFF, -72]],
            column_names=["observed_at", "node", "mac", "rssi"],
            settings={"async_insert": 1, "wait_for_async_insert": 0},
        )


if __name__ == "__main__":
    unittest.main()
