import sqlite3
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

DATABASE_PATH = "/data/middlines.db"

db: sqlite3.Connection


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    global db
    db = sqlite3.connect(DATABASE_PATH)
    db.row_factory = sqlite3.Row
    yield
    db.close()


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
