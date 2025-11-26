import sqlite3
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from datetime import datetime
from typing import Literal

from fastapi import FastAPI
from loguru import logger
from pydantic import BaseModel

DATABASE_PATH = "/data/middlines.db"

# Compare last N minutes to previous N minutes for trend
TREND_WINDOW_MINUTES = 10

db: sqlite3.Connection


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    global db
    db = sqlite3.connect(DATABASE_PATH)
    db.row_factory = sqlite3.Row
    logger.info(f"Connected to database at {DATABASE_PATH}")
    yield
    db.close()
    logger.info("Database connection closed")


app = FastAPI(lifespan=lifespan, root_path="/api")


@app.get("/health")
def health() -> str:
    return "Ok"


class DataPoint(BaseModel):
    timestamp: datetime
    busyness_percentage: float


class LocationStatus(BaseModel):
    location: str
    timestamp: datetime
    busyness_percentage: float
    vs_typical_percentage: float
    trend: Literal["Increasing", "Steady", "Decreasing"]
    today_data: list[DataPoint]


@app.get("/current")
def get_current() -> list[LocationStatus]:
    return []
