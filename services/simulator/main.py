import math
import os
import random
import sqlite3
from datetime import datetime, timedelta
from time import sleep
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt
from loguru import logger
from paho.mqtt.enums import CallbackAPIVersion

TIMEZONE = ZoneInfo(os.environ.get("TZ", "America/New_York"))

MQTT_HOST = "mosquitto"
MQTT_PORT = 1883
DATABASE_PATH = "/data/middlines.db"
PUBLISH_INTERVAL_SECONDS = 60

TEST_LOCATION = "Simulated Test"


def _is_weekend(current: datetime) -> bool:
    # weekday(): Monday=0, Sunday=6
    return current.weekday() >= 5


def _weekday_count(hour: int, minute: int) -> int:
    # Closed hours: before 7 AM or after 8 PM
    if hour < 7 or hour >= 20:
        noise = random.randint(-2, 2)
        return max(0, 3 + noise)

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


def _weekend_count(hour: int, minute: int) -> int:
    # Closed hours: before 7 AM or after 8 PM
    if hour < 7 or hour >= 20:
        noise = random.randint(-2, 2)
        return max(0, 3 + noise)

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


def generate_count(current: datetime) -> int:
    hour = current.hour
    minute = current.minute

    if _is_weekend(current):
        base = _weekend_count(hour, minute)
    else:
        base = _weekday_count(hour, minute)

    return max(0, int(base))


def seed_historical_data() -> None:
    conn = sqlite3.connect(DATABASE_PATH, timeout=5.0)

    # Clear out any previous generated data for the test location
    conn.execute(
        "DELETE FROM counts WHERE location = ?",
        (TEST_LOCATION,),
    )
    conn.commit()

    now = datetime.now(TIMEZONE).replace(second=0, microsecond=0)
    start = now - timedelta(days=30)

    logger.info(f"Seeding historical data for {TEST_LOCATION} from {start} to {now}")

    rows: list[tuple[str, int, str]] = []

    current = start
    while current < now:
        timestamp = current.isoformat(sep=" ", timespec="seconds")
        count = generate_count(current)
        rows.append((TEST_LOCATION, count, timestamp))
        current += timedelta(seconds=PUBLISH_INTERVAL_SECONDS)

    conn.executemany(
        "INSERT INTO counts (location, count, timestamp) VALUES (?, ?, ?)",
        rows,
    )
    conn.commit()
    conn.close()

    logger.info(f"Seeded {len(rows)} historical data points for {TEST_LOCATION}")


def run_live_simulation() -> None:
    client = mqtt.Client(CallbackAPIVersion.VERSION2)

    logger.info(f"Connecting simulator to MQTT at {MQTT_HOST}:{MQTT_PORT}")
    client.connect(MQTT_HOST, MQTT_PORT)
    client.loop_start()

    try:
        while True:
            current = datetime.now(TIMEZONE).replace(second=0, microsecond=0)

            count = generate_count(current)
            topic = f"middlines/{TEST_LOCATION}/count"
            client.publish(topic, str(count))
            logger.info(
                f"Published simulated count {count} for {TEST_LOCATION} at {current.isoformat()}"
            )

            sleep(PUBLISH_INTERVAL_SECONDS)
    finally:
        client.loop_stop()
        client.disconnect()


def main() -> None:
    logger.info("Simulator service starting")

    seed_historical_data()
    run_live_simulation()


if __name__ == "__main__":
    main()
