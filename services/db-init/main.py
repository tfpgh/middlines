import sqlite3

from loguru import logger

DATABASE_PATH = "/data/middlines.db"

# EMA smoothing parameter
EMA_ALPHA = 0.20


def init_db() -> None:
    logger.info(f"Initializing database at {DATABASE_PATH}")
    conn = sqlite3.connect(DATABASE_PATH)

    # Enable WAL mode for better concurrent read/write performance
    conn.execute("PRAGMA journal_mode=WAL")
    logger.info("WAL mode enabled")

    # Create counts table
    conn.execute("""
        CREATE TABLE IF NOT EXISTS counts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            location TEXT NOT NULL,
            count INTEGER NOT NULL,
            timestamp TEXT NOT NULL
        );
    """)
    conn.commit()
    logger.info("Counts table ready")

    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_counts_location_timestamp
        ON counts(location, timestamp);
    """)
    conn.commit()
    logger.info("Counts index ready")

    # Create (or recreate) the smoothed view
    conn.execute("DROP VIEW IF EXISTS smoothed_counts")
    conn.execute(f"""
        CREATE VIEW smoothed_counts AS
        WITH RECURSIVE
        base AS (
            SELECT
                location, timestamp, count,
                ROW_NUMBER() OVER (
                    PARTITION BY location ORDER BY timestamp
                ) AS rn
            FROM counts
        ),
        ema AS (
            -- Seed: first row per location in the cutoff window
            SELECT location, timestamp, rn,
                   CAST(count AS REAL) AS smoothed_count
            FROM base
            WHERE rn = 1

            UNION ALL

            -- Recursive step: EMA formula
            SELECT
                b.location, b.timestamp, b.rn,
                {EMA_ALPHA} * b.count + {1 - EMA_ALPHA} * e.smoothed_count
            FROM base b
            JOIN ema e ON b.location = e.location AND b.rn = e.rn + 1
        )
        SELECT location, timestamp, smoothed_count
        FROM ema;
    """)
    conn.commit()
    logger.info("Smoothed counts view created")

    conn.close()
    logger.info("Database initialization complete")


if __name__ == "__main__":
    init_db()
