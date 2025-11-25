import sqlite3
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from loguru import logger

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

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["GET"],
    allow_headers=["*"],
)


@app.get("/health")
def health() -> str:
    return "Ok"
