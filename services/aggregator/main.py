import os
import sqlite3
from datetime import datetime, timedelta
from time import sleep, time
from zoneinfo import ZoneInfo

import schedule
from loguru import logger

DATABASE_PATH = "/data/middlines.db"

TIMEZONE = ZoneInfo(os.environ.get("TZ", "America/New_York"))

# The max count isn't a true max, it's at a percentile. Here we choose the 99th percentile.
MAX_PERCENTILE_THRESHOLD = 0.99
# Previous days considered to calculate max count and time buckets
LOOKBACK_LENGTH = 45
# How many times the baseline a count can be for the hall to be considered closed
CLOSED_THRESHOLD = 1.5
# Average count bucket sizes in minutes
TIME_BUCKET_SIZE = int(os.environ.get("TIME_BUCKET_SIZE", "10"))


# Compute baselines from last night's 1-4am readings
def compute_baselines() -> None:
    logger.info("Computing baseline")

    conn = sqlite3.connect(DATABASE_PATH, timeout=5.0)

    cutoff_time = datetime.now(TIMEZONE) - timedelta(days=1)
    cutoff_str = cutoff_time.isoformat(sep=" ", timespec="seconds")

    conn.execute(
        """
        INSERT OR REPLACE INTO baseline (location, baseline_count)
        SELECT
            location,
            AVG(smoothed_count) as baseline_count
        FROM smoothed_counts
        WHERE CAST(strftime('%H', timestamp) as INTEGER) IN (1, 2, 3)
            AND timestamp > ?
        GROUP BY location
    """,
        (cutoff_str,),
    )

    location_count = conn.total_changes
    conn.commit()
    conn.close()

    logger.info(f"Baseline calculated for {location_count} locations")


# Compute max count (at MAX_PERCENTILE_THRESHOLD) for each location
def compute_max_counts() -> None:
    logger.info("Computing max counts")

    conn = sqlite3.connect(DATABASE_PATH, timeout=5.0)

    lookback_time = datetime.now(TIMEZONE) - timedelta(days=LOOKBACK_LENGTH)
    lookback_str = lookback_time.isoformat(sep=" ", timespec="seconds")

    conn.execute(
        """
            WITH ranked_counts AS (
                SELECT
                    c.location,
                    c.smoothed_count,
                    ROW_NUMBER() OVER (PARTITION BY c.location ORDER BY c.smoothed_count DESC) as rank,
                    COUNT(*) OVER (PARTITION BY c.location) as total_count
                FROM smoothed_counts c
                JOIN baseline b ON c.location = b.location
                WHERE c.timestamp > ?
                  AND c.smoothed_count > b.baseline_count * ?
            ),
            percentile_max AS (
                SELECT
                    location,
                    MIN(smoothed_count) as max_count
                FROM ranked_counts
                WHERE rank <= CAST(total_count * (1 - ?) AS INTEGER) + 1
                GROUP BY location
            )
            INSERT OR REPLACE INTO max_count (location, baseline_adjusted_max_count)
            SELECT
                p.location,
                p.max_count - COALESCE(b.baseline_count, 0) as baseline_adjusted_max_count
            FROM percentile_max p
            LEFT JOIN baseline b ON p.location = b.location
        """,
        (lookback_str, CLOSED_THRESHOLD, MAX_PERCENTILE_THRESHOLD),
    )

    location_count = conn.total_changes
    conn.commit()
    conn.close()

    logger.info(
        f"Max ({MAX_PERCENTILE_THRESHOLD * 100}%) count calculated for {location_count} locations"
    )


# Compute average count by location/day/time bucket for "vs typical" stat
def compute_time_averages() -> None:
    logger.info("Computing time averages")

    conn = sqlite3.connect(DATABASE_PATH, timeout=5.0)

    lookback_time = datetime.now(TIMEZONE) - timedelta(days=LOOKBACK_LENGTH)
    lookback_str = lookback_time.isoformat(sep=" ", timespec="seconds")

    conn.execute(
        """
            INSERT OR REPLACE INTO time_averages
                (location, day_of_week, time_bucket, mean_count)
            SELECT
                c.location,
                CAST(strftime('%w', c.timestamp) AS INTEGER) as day_of_week,
                (CAST(strftime('%H', c.timestamp) AS INTEGER) * 60 +
                 (CAST(strftime('%M', c.timestamp) AS INTEGER) / ?) * ?) as time_bucket,
                AVG(c.smoothed_count) as mean_count
            FROM smoothed_counts c
            JOIN baseline b ON c.location = b.location
            WHERE c.timestamp > ?
              AND c.smoothed_count > b.baseline_count * ?
            GROUP BY c.location, day_of_week, time_bucket
        """,
        (TIME_BUCKET_SIZE, TIME_BUCKET_SIZE, lookback_str, CLOSED_THRESHOLD),
    )

    bucket_count = conn.total_changes
    conn.commit()
    conn.close()

    logger.info(
        f"Time averages calculated for {bucket_count} location/day/time buckets"
    )


def run_aggregation() -> None:
    logger.info("Starting aggregation run")
    start_time = time()

    try:
        compute_baselines()
        compute_max_counts()
        compute_time_averages()

        elapsed = time() - start_time
        logger.info(f"Aggregation completed successfully in {round(elapsed, 2)}s")
    except Exception as e:
        logger.error(f"Aggregation failed: {e}")


def main() -> None:
    logger.info("Aggregator service starting")

    logger.info("Running first initial aggregation")
    run_aggregation()

    logger.info("Sleeping for 10 seconds")
    sleep(10)

    logger.info(
        "Running second initial aggregation to aggregate potentially missing test data"
    )
    run_aggregation()

    schedule.every(1).minutes.do(run_aggregation)
    logger.info("Scheduled aggregation to run every minute")

    while True:
        schedule.run_pending()
        sleep(1)


if __name__ == "__main__":
    main()
