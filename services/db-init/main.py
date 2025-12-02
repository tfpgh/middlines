import sqlite3

from loguru import logger

DATABASE_PATH = "/data/middlines.db"

# Number of rows to smooth over. 20 rows at 30 second intervals = 10 min rolling average
SMOOTHING_WINDOW_ROWS = 20


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
        SELECT
            location,
            timestamp,
            AVG(count) OVER (
                PARTITION BY location
                ORDER BY timestamp
                ROWS BETWEEN {SMOOTHING_WINDOW_ROWS - 1} PRECEDING AND CURRENT ROW
            ) as smoothed_count
        FROM counts
    """)
    conn.commit()
    logger.info("Smoothed counts view created")

    conn.close()
    logger.info("Database initialization complete")


if __name__ == "__main__":
    init_db()
