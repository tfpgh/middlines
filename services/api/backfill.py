"""Backfill minute aggregates without pausing ingestion; safe to rerun a range."""

import argparse
from datetime import UTC, datetime, timedelta

from store import ClickHouseStore


def timestamp(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Expected an ISO 8601 timestamp") from exc
    if parsed.tzinfo is None:
        raise argparse.ArgumentTypeError("Timestamp must include a UTC offset")
    return parsed.astimezone(UTC)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", type=timestamp, required=True)
    parser.add_argument("--end", type=timestamp, required=True)
    parser.add_argument("--chunk-minutes", type=int, default=60)
    args = parser.parse_args()
    if args.start >= args.end or args.chunk_minutes <= 0:
        parser.error("Require start < end and a positive chunk duration")
    if args.end > datetime.now(UTC):
        parser.error("End must not be in the future")
    store = ClickHouseStore.from_env()
    try:
        start = args.start
        while start < args.end:
            end = min(start + timedelta(minutes=args.chunk_minutes), args.end)
            store.backfill_minute_counts(start, end)
            print(f"Completed [{start.isoformat()}, {end.isoformat()})", flush=True)
            start = end
    finally:
        store.close()


if __name__ == "__main__":
    main()
