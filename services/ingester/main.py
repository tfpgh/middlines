import math
import random
import sqlite3
from typing import Any

import paho.mqtt.client as mqtt
from loguru import logger
from paho.mqtt.client import ConnectFlags, MQTTMessage
from paho.mqtt.enums import CallbackAPIVersion
from paho.mqtt.properties import Properties
from paho.mqtt.reasoncodes import ReasonCode

MQTT_HOST = "mosquitto"
MQTT_PORT = 1883
DATABASE_PATH = "/data/middlines.db"
TOPIC = "middlines/+/count"


# Seeds the DB with some realistic test data
def seed_db(conn: sqlite3.Connection) -> None:
    def get_count(hour: int, minute: int) -> int:
        # Base pattern with peaks at meals
        if 7 <= hour <= 9:  # Breakfast
            base = 35
        elif 11 <= hour <= 13:  # Lunch
            base = 55
        elif 17 <= hour <= 19:  # Dinner
            base = 50
        else:  # Off-peak
            base = 15

        # Add minute to minute variation
        t = hour + minute / 60
        wave = math.sin(t * 0.5) * 5
        noise = random.randint(-8, 8)

        return max(0, int(base + wave + noise))

    data: list[tuple[str, int, str]] = []
    for hour in range(7, 22):
        for minute in range(60):
            timestamp = f"2025-11-01 {hour:02d}:{minute:02d}:00"
            data.append(("Test", get_count(hour, minute), timestamp))

    conn.executemany(
        "INSERT INTO counts (location, count, timestamp) VALUES (?, ?, ?)", data
    )
    conn.commit()
    logger.info(f"Seeded {len(data)} test data points")


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
                timestamp TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );""")
        conn.commit()

        seed_db(conn)
        logger.info(f"Database initialized at {DATABASE_PATH}")
    else:
        logger.info(f"Database already exists at {DATABASE_PATH}")

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

        conn = sqlite3.connect(DATABASE_PATH)
        conn.execute(
            "INSERT INTO counts (location, count) VALUES (?, ?)",
            (location, count),
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
