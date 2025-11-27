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

        conn = sqlite3.connect(DATABASE_PATH, timeout=5.0)
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
    client = mqtt.Client(CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message

    logger.info(f"Connecting to {MQTT_HOST}:{MQTT_PORT}")
    client.connect(MQTT_HOST, MQTT_PORT)
    client.loop_forever()


if __name__ == "__main__":
    main()
