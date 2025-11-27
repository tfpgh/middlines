import os
import sqlite3
from datetime import datetime
from typing import Any
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt
from loguru import logger
from paho.mqtt.client import ConnectFlags, MQTTMessage
from paho.mqtt.enums import CallbackAPIVersion
from paho.mqtt.properties import Properties
from paho.mqtt.reasoncodes import ReasonCode

TIMEZONE = ZoneInfo(os.environ.get("TZ", "America/New_York"))

MQTT_HOST = "mosquitto"
MQTT_PORT = 1883
DATABASE_PATH = "/data/middlines.db"
TOPIC = "middlines/+/count"

# Number of rows to smooth over in smoothed_counts. 5 rows at 2 min intervals is 10 min average
SMOOTHING_WINDOW_ROWS = 5


def init_db() -> None:
    conn = sqlite3.connect(DATABASE_PATH)

    exists = conn.execute("""
            SELECT name FROM sqlite_master
            WHERE type='table' AND name='counts'
        """).fetchone()

    if not exists:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS counts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                location TEXT NOT NULL,
                count INTEGER NOT NULL,
                timestamp TEXT NOT NULL
            );""")
        conn.commit()

        logger.info(f"Database initialized at {DATABASE_PATH}")
    else:
        logger.info(f"Database already exists at {DATABASE_PATH}")

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


def on_connect(
    client: mqtt.Client,
    _userdata: Any,
    _flags: ConnectFlags,
    _rc: ReasonCode,
    _properties: Properties | None = None,
) -> None:
    logger.info(f"Connected to MQTT broker, subscribing to {TOPIC}")
    client.subscribe(TOPIC)


def on_message(
    _client: mqtt.Client,
    _userdata: Any,
    msg: MQTTMessage,
) -> None:
    try:
        # Topic format is middlines/{location}/count
        location = msg.topic.split("/")[1]
        count = int(msg.payload.decode())

        local_now = datetime.now(TIMEZONE)
        timestamp = local_now.isoformat(sep=" ", timespec="seconds")

        conn = sqlite3.connect(DATABASE_PATH)
        conn.execute(
            "INSERT INTO counts (location, count, timestamp) VALUES (?, ?, ?)",
            (location, count, timestamp),
        )
        conn.commit()
        conn.close()

        logger.info(f"Saved count {location}: {count} devices")
    except Exception as e:
        logger.error(f"Message handling error: {e}")


def main() -> None:
    init_db()

    client = mqtt.Client(CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    logger.info(f"Connecting to {MQTT_HOST}:{MQTT_PORT}")
    client.connect(MQTT_HOST, MQTT_PORT)
    client.loop_forever()


if __name__ == "__main__":
    main()
