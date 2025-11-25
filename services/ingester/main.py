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
    # Check if day in November 2025 is a weekend
    def is_weekend_day(day: int) -> bool:
        # November 2025: 1st is Saturday
        # Weekends: 1-2, 8-9, 15-16, 22-23, 29-30
        return day in [1, 2, 8, 9, 15, 16, 22, 23, 29, 30]

    # Generate weekday count with meal rushes
    def get_weekday_count(hour: int, minute: int) -> int:
        t = hour + minute / 60.0
        base_traffic = 10

        # Breakfast: 7-10 AM, peak 8:00
        breakfast = 40 * math.exp(-((t - 8.0) ** 2) / (2 * 0.8**2))

        # Lunch: 11 AM-2 PM, peak 12:15
        lunch = 60 * math.exp(-((t - 12.25) ** 2) / (2 * 1.0**2))

        # Dinner: 5-8 PM, peak 6:30
        dinner = 55 * math.exp(-((t - 18.5) ** 2) / (2 * 1.0**2))

        total = base_traffic + breakfast + lunch + dinner
        minute_variation = math.sin(minute * 0.1) * 3
        noise = random.randint(-5, 5)

        return max(0, int(total + minute_variation + noise))

    # Generate weekend count with lower and flatter traffic
    def get_weekend_count(hour: int, minute: int) -> int:
        t = hour + minute / 60.0
        base_traffic = 5

        # Late brunch: 10 AM-2 PM, peak 12:00, wider curve
        brunch = 35 * math.exp(-((t - 12.0) ** 2) / (2 * 1.5**2))

        # Dinner: 5-8 PM, peak 6:00, less pronounced
        dinner = 30 * math.exp(-((t - 18.0) ** 2) / (2 * 1.2**2))

        total = base_traffic + brunch + dinner
        minute_variation = math.sin(minute * 0.1) * 2
        noise = random.randint(-4, 4)

        return max(0, int(total + minute_variation + noise))

    def get_count(hour: int, minute: int, is_weekend: bool) -> int:
        if is_weekend:
            return get_weekend_count(hour, minute)
        else:
            return get_weekday_count(hour, minute)

    data: list[tuple[str, int, str]] = []
    for day in range(1, 31):  # November 1-30
        is_weekend = is_weekend_day(day)
        for hour in range(7, 22):
            for minute in range(60):
                timestamp = f"2025-11-{day:02d} {hour:02d}:{minute:02d}:00"
                count = get_count(hour, minute, is_weekend)
                data.append(("Generated Test", count, timestamp))

    conn.executemany(
        "INSERT INTO counts (location, count, timestamp) VALUES (?, ?, ?)", data
    )
    conn.commit()
    logger.info(f"Seeded {len(data)} test data points for November 2025")


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
