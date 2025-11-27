import sqlite3

from loguru import logger

DATABASE_PATH = "/data/middlines.db"

# Number of rows to smooth over in smoothed_counts. 5 rows at 2 min intervals is 10 min average
SMOOTHING_WINDOW_ROWS = 5


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

    # Create aggregation tables
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS baseline (
            location TEXT PRIMARY KEY,
            baseline_count REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS max_count (
            location TEXT PRIMARY KEY,
            baseline_adjusted_max_count REAL NOT NULL
        );

        CREATE TABLE IF NOT EXISTS time_averages (
            location TEXT NOT NULL,
            day_of_week INTEGER NOT NULL,
            time_bucket INTEGER NOT NULL,
            mean_count REAL NOT NULL,
            PRIMARY KEY (location, day_of_week, time_bucket)
        );
    """)
    conn.commit()
    logger.info("Aggregation tables ready")

    conn.close()
    logger.info("Database initialization complete")


if __name__ == "__main__":
    init_db()
