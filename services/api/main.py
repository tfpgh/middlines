import hashlib
import hmac
import os
import secrets
import sqlite3
from base64 import urlsafe_b64decode, urlsafe_b64encode
from collections.abc import AsyncIterator, Generator
from contextlib import asynccontextmanager
from datetime import UTC, datetime, timedelta
from html import escape
from pathlib import Path
from time import time
from typing import Annotated, Literal, cast
from zoneinfo import ZoneInfo

from fastapi import (
    Depends,
    FastAPI,
    File,
    Form,
    Header,
    HTTPException,
    Request,
    UploadFile,
)
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse, HTMLResponse, RedirectResponse
from loguru import logger
from pydantic import BaseModel

DATABASE_PATH = "/data/middlines.db"
CONTROL_DATABASE_PATH = "/data/device_control.db"
ARTIFACTS_DIR = Path("/data/ota")
TIMEZONE = ZoneInfo(os.environ.get("TZ", "America/New_York"))

ADMIN_USERNAME = os.environ.get("MIDDLINES_ADMIN_USERNAME", "admin")
ADMIN_PASSWORD = os.environ.get("MIDDLINES_ADMIN_PASSWORD", "changeme")
SESSION_SECRET = os.environ.get("MIDDLINES_SESSION_SECRET", "dev-session-secret")

DEFAULT_NODES = ("ross", "proctor", "atwater")
DEFAULT_POLL_INTERVAL_S = 300
SESSION_COOKIE = "middlines_admin"
PUBLIC_API_PREFIX = "/api"

# Cache TTL in seconds
CACHE_TTL = 30

# Trend: compare current count to N rows back
TREND_LOOKBACK_ROWS = 20
# Trend: percentage change threshold to determine increasing/decreasing
TREND_THRESHOLD = 0.07

# Aggregation: time bucket size in minutes
TIME_BUCKET_SIZE = 2
# Aggregation: days of historical data to consider
LOOKBACK_DAYS = 45
# Aggregation: percentile for max count calculation (0.99 = 99th percentile)
MAX_PERCENTILE = 0.9995
# Aggregation: multiplier of baseline below which location is considered closed
CLOSED_THRESHOLD = 1.5

# Trend: minimum busyness percentage to report a trend (below this, trend is None)
TREND_MIN_BUSYNESS = 10.0


class DataPoint(BaseModel):
    timestamp: datetime
    busyness_percentage: float | None


class LocationStatus(BaseModel):
    location: str
    timestamp: datetime
    busyness_percentage: float | None
    vs_typical_percentage: float | None
    trend: Literal["Increasing", "Steady", "Decreasing"] | None
    today_data: list[DataPoint]


class SmoothedCount(BaseModel):
    location: str
    timestamp: datetime
    count: float


class LocationAggregates(BaseModel):
    baseline: float
    max_count: float
    time_averages: dict[tuple[bool, int], float]


def utc_now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def ensure_directories() -> None:
    ARTIFACTS_DIR.mkdir(parents=True, exist_ok=True)


def get_control_db_connection() -> sqlite3.Connection:
    db = sqlite3.connect(CONTROL_DATABASE_PATH, timeout=5.0)
    db.row_factory = sqlite3.Row
    return db


def init_control_db() -> None:
    db = get_control_db_connection()
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS nodes (
            node TEXT PRIMARY KEY,
            token TEXT NOT NULL DEFAULT '',
            poll_interval_s INTEGER NOT NULL DEFAULT 300,
            current_version TEXT,
            last_seen_at TEXT,
            last_manifest_fetch_at TEXT,
            last_ip TEXT,
            updated_at TEXT NOT NULL
        )
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS firmware_artifacts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT NOT NULL UNIQUE,
            original_filename TEXT NOT NULL,
            version TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            size_bytes INTEGER NOT NULL,
            uploaded_at TEXT NOT NULL
        )
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS node_desired_state (
            node TEXT PRIMARY KEY,
            target_firmware_id INTEGER,
            restart_nonce TEXT,
            updated_at TEXT NOT NULL,
            FOREIGN KEY(node) REFERENCES nodes(node),
            FOREIGN KEY(target_firmware_id) REFERENCES firmware_artifacts(id)
        )
        """
    )
    now = utc_now()
    for node in DEFAULT_NODES:
        db.execute(
            """
            INSERT INTO nodes (node, updated_at)
            VALUES (?, ?)
            ON CONFLICT(node) DO NOTHING
            """,
            (node, now),
        )
        db.execute(
            """
            INSERT INTO node_desired_state (node, updated_at)
            VALUES (?, ?)
            ON CONFLICT(node) DO NOTHING
            """,
            (node, now),
        )
    db.commit()
    db.close()


def _compute_aggregates(counts: list[SmoothedCount]) -> dict[str, LocationAggregates]:
    now = datetime.now(TIMEZONE)
    yesterday = now - timedelta(days=1)
    lookback_start = now - timedelta(days=LOOKBACK_DAYS)

    by_location: dict[str, list[SmoothedCount]] = {}
    for c in counts:
        by_location.setdefault(c.location, []).append(c)

    result: dict[str, LocationAggregates] = {}
    for location, location_counts in by_location.items():
        baseline_counts = [
            c.count
            for c in location_counts
            if c.timestamp > yesterday and c.timestamp.hour in set(range(1, 4))
        ]
        baseline = sum(baseline_counts) / len(baseline_counts) if baseline_counts else 0

        open_counts = sorted(
            c.count
            for c in location_counts
            if c.timestamp > lookback_start and c.count > baseline * CLOSED_THRESHOLD
        )
        if open_counts:
            percentile_idx = int(len(open_counts) * MAX_PERCENTILE)
            percentile_idx = min(percentile_idx, len(open_counts) - 1)
            max_count = open_counts[percentile_idx] - baseline
        else:
            max_count = 0

        time_buckets: dict[tuple[bool, int], list[float]] = {}
        for c in location_counts:
            if c.timestamp <= lookback_start:
                continue
            if c.count <= baseline * CLOSED_THRESHOLD:
                continue
            is_weekend = c.timestamp.weekday() >= 5
            minutes = (
                c.timestamp.hour * 60
                + (c.timestamp.minute // TIME_BUCKET_SIZE) * TIME_BUCKET_SIZE
            )
            time_buckets.setdefault((is_weekend, minutes), []).append(c.count)

        result[location] = LocationAggregates(
            baseline=baseline,
            max_count=max_count,
            time_averages={k: sum(v) / len(v) for k, v in time_buckets.items()},
        )

    return result


def _calculate_busyness(
    count: float | None,
    baseline: float | None,
    max_count: float | None,
) -> float | None:
    if count is None or baseline is None or max_count is None or max_count <= 0:
        return None
    busyness = ((count - baseline) / max_count) * 100
    return max(0.0, min(100.0, busyness))


def _build_location_status(db: sqlite3.Connection) -> list[LocationStatus]:
    now = datetime.now(TIMEZONE)
    midnight_today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    lookback_start = now - timedelta(days=LOOKBACK_DAYS)

    rows = db.execute(
        """
        SELECT location, timestamp, smoothed_count
        FROM smoothed_counts
        WHERE timestamp >= ?
        ORDER BY location, timestamp
        """,
        (lookback_start.isoformat(sep=" ", timespec="seconds"),),
    ).fetchall()

    if not rows:
        raise HTTPException(status_code=503, detail="No data available")

    counts = [
        SmoothedCount(
            location=row["location"],
            timestamp=datetime.fromisoformat(row["timestamp"]),
            count=row["smoothed_count"],
        )
        for row in rows
    ]

    aggregates = _compute_aggregates(counts)
    by_location: dict[str, list[SmoothedCount]] = {}
    for c in counts:
        by_location.setdefault(c.location, []).append(c)

    results: list[LocationStatus] = []
    for location, location_counts in sorted(by_location.items()):
        agg = aggregates.get(location)
        if not agg:
            continue

        latest = location_counts[-1]
        past_count = (
            location_counts[-1 - TREND_LOOKBACK_ROWS].count
            if len(location_counts) > TREND_LOOKBACK_ROWS
            else None
        )
        busyness = _calculate_busyness(latest.count, agg.baseline, agg.max_count)

        is_weekend = latest.timestamp.weekday() >= 5
        minutes = (
            latest.timestamp.hour * 60
            + (latest.timestamp.minute // TIME_BUCKET_SIZE) * TIME_BUCKET_SIZE
        )
        typical = agg.time_averages.get((is_weekend, minutes))
        vs_typical = (
            ((latest.count - typical) / typical) * 100
            if typical and typical > 0
            else None
        )

        trend: Literal["Increasing", "Steady", "Decreasing"] | None = None
        if (
            busyness is not None
            and busyness >= TREND_MIN_BUSYNESS
            and past_count
            and past_count > 0
        ):
            change = (latest.count - past_count) / past_count
            if change > TREND_THRESHOLD:
                trend = "Increasing"
            elif change < -TREND_THRESHOLD:
                trend = "Decreasing"
            else:
                trend = "Steady"

        results.append(
            LocationStatus(
                location=location,
                timestamp=latest.timestamp,
                busyness_percentage=busyness,
                vs_typical_percentage=vs_typical,
                trend=trend,
                today_data=[
                    DataPoint(
                        timestamp=c.timestamp,
                        busyness_percentage=_calculate_busyness(
                            c.count, agg.baseline, agg.max_count
                        ),
                    )
                    for c in location_counts
                    if c.timestamp >= midnight_today
                ],
            )
        )

    return results


def sign_session_value(value: str) -> str:
    signature = hmac.new(
        SESSION_SECRET.encode(), value.encode(), hashlib.sha256
    ).hexdigest()
    return urlsafe_b64encode(f"{value}:{signature}".encode()).decode()


def decode_session_value(raw_value: str | None) -> str | None:
    if not raw_value:
        return None
    try:
        decoded = urlsafe_b64decode(raw_value.encode()).decode()
        value, signature = decoded.rsplit(":", 1)
    except Exception:
        return None
    expected = hmac.new(
        SESSION_SECRET.encode(), value.encode(), hashlib.sha256
    ).hexdigest()
    if not hmac.compare_digest(signature, expected):
        return None
    return value


def require_admin(request: Request) -> None:
    session = decode_session_value(request.cookies.get(SESSION_COOKIE))
    if session != ADMIN_USERNAME:
        raise HTTPException(
            status_code=303, headers={"Location": f"{PUBLIC_API_PREFIX}/admin/login"}
        )


def html_page(title: str, body: str) -> HTMLResponse:
    return HTMLResponse(
        f"""
        <!doctype html>
        <html lang=\"en\">
        <head>
          <meta charset=\"utf-8\">
          <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
          <title>{escape(title)}</title>
          <style>
            body {{ font-family: system-ui, sans-serif; margin: 0; background: #f5f5f5; color: #111; }}
            main {{ max-width: 960px; margin: 0 auto; padding: 24px; }}
            h1, h2, h3 {{ margin-top: 0; }}
            .topbar {{ display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }}
            .card {{ background: white; border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); }}
            .grid {{ display: grid; gap: 16px; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); }}
            label {{ display: block; font-weight: 600; margin-bottom: 8px; }}
            input, select, button {{ font: inherit; padding: 8px 10px; border-radius: 8px; border: 1px solid #ccc; width: 100%; box-sizing: border-box; }}
            button {{ cursor: pointer; background: #111; color: white; border: none; }}
            button.secondary {{ background: #666; }}
            form {{ margin: 0; }}
            .row {{ display: flex; gap: 8px; align-items: end; }}
            .row > * {{ flex: 1; }}
            table {{ width: 100%; border-collapse: collapse; }}
            th, td {{ text-align: left; padding: 10px 8px; border-bottom: 1px solid #e5e5e5; }}
            .mono {{ font-family: ui-monospace, monospace; }}
            .muted {{ color: #555; }}
            a {{ color: #0a58ca; text-decoration: none; }}
          </style>
        </head>
        <body>
          <main>{body}</main>
        </body>
        </html>
        """
    )


def render_admin_shell(title: str, content: str) -> HTMLResponse:
    body = (
        "<div class='topbar'><div>"
        f"<h1>{escape(title)}</h1>"
        "<div class='muted'>Node control and OTA management</div>"
        "</div>"
        f"<form method='post' action='{PUBLIC_API_PREFIX}/admin/logout'><button class='secondary'>Log out</button></form>"
        "</div>"
        f"<p><a href='{PUBLIC_API_PREFIX}/admin'>Dashboard</a></p>"
        f"{content}"
    )
    return html_page(title, body)


def fetch_admin_dashboard_data() -> tuple[list[sqlite3.Row], list[sqlite3.Row]]:
    db = get_control_db_connection()
    nodes = db.execute(
        """
        SELECT n.node, n.token, n.poll_interval_s, n.current_version, n.last_seen_at,
               n.last_manifest_fetch_at, n.last_ip,
               ds.restart_nonce, fa.version AS target_version
        FROM nodes n
        LEFT JOIN node_desired_state ds ON ds.node = n.node
        LEFT JOIN firmware_artifacts fa ON fa.id = ds.target_firmware_id
        ORDER BY n.node
        """
    ).fetchall()
    artifacts = db.execute(
        """
        SELECT id, filename, original_filename, version, sha256, size_bytes, uploaded_at
        FROM firmware_artifacts
        ORDER BY uploaded_at DESC, id DESC
        """
    ).fetchall()
    db.close()
    return cast(list[sqlite3.Row], nodes), cast(list[sqlite3.Row], artifacts)


def fetch_node_detail(node: str) -> tuple[sqlite3.Row, list[sqlite3.Row]]:
    db = get_control_db_connection()
    row = db.execute(
        """
        SELECT n.node, n.token, n.poll_interval_s, n.current_version, n.last_seen_at,
               n.last_manifest_fetch_at, n.last_ip,
               ds.restart_nonce, fa.id AS target_firmware_id, fa.version AS target_version
        FROM nodes n
        LEFT JOIN node_desired_state ds ON ds.node = n.node
        LEFT JOIN firmware_artifacts fa ON fa.id = ds.target_firmware_id
        WHERE n.node = ?
        """,
        (node,),
    ).fetchone()
    if row is None:
        db.close()
        raise HTTPException(status_code=404, detail="Unknown node")
    artifacts = db.execute(
        """
        SELECT id, version, filename, sha256, uploaded_at
        FROM firmware_artifacts
        ORDER BY uploaded_at DESC, id DESC
        """
    ).fetchall()
    db.close()
    return row, cast(list[sqlite3.Row], artifacts)


def update_node_seen(node: str, version: str | None, client_ip: str | None) -> None:
    db = get_control_db_connection()
    db.execute(
        """
        UPDATE nodes
        SET current_version = COALESCE(?, current_version),
            last_seen_at = ?,
            last_manifest_fetch_at = ?,
            last_ip = ?,
            updated_at = ?
        WHERE node = ?
        """,
        (version or None, utc_now(), utc_now(), client_ip, utc_now(), node),
    )
    db.commit()
    db.close()


def get_node_state(node: str) -> sqlite3.Row | None:
    db = get_control_db_connection()
    row = db.execute(
        """
        SELECT n.node, n.token, n.poll_interval_s, ds.restart_nonce,
               fa.version, fa.filename, fa.sha256
        FROM nodes n
        LEFT JOIN node_desired_state ds ON ds.node = n.node
        LEFT JOIN firmware_artifacts fa ON fa.id = ds.target_firmware_id
        WHERE n.node = ?
        """,
        (node,),
    ).fetchone()
    db.close()
    return row


_cache: tuple[float, list[LocationStatus]] | None = None


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    ensure_directories()
    init_control_db()
    logger.info(
        f"API starting, database at {DATABASE_PATH}, control db at {CONTROL_DATABASE_PATH}"
    )
    yield
    logger.info("API shutting down")


app = FastAPI(lifespan=lifespan, root_path="/api")
app.add_middleware(GZipMiddleware)


def get_db() -> Generator[sqlite3.Connection]:
    db = sqlite3.connect(DATABASE_PATH, timeout=5.0)
    db.row_factory = sqlite3.Row
    try:
        yield db
    finally:
        db.close()


@app.get("/health")
def health() -> str:
    return "Ok"


@app.get("/current")
def get_current(
    db: Annotated[sqlite3.Connection, Depends(get_db)],
) -> list[LocationStatus]:
    global _cache
    now = time()
    if _cache is not None:
        cached_time, cached_data = _cache
        if now - cached_time < CACHE_TTL:
            return cached_data
    results = _build_location_status(db)
    _cache = (now, results)
    return results


@app.get("/node/{node}/manifest")
def get_node_manifest(
    request: Request,
    node: str,
    authorization: Annotated[str | None, Header()] = None,
    x_middlines_version: Annotated[str | None, Header()] = None,
) -> dict[str, object | None]:
    state = get_node_state(node)
    if state is None:
        raise HTTPException(status_code=404, detail="Unknown node")

    expected_token = state["token"]
    if not expected_token:
        raise HTTPException(status_code=403, detail="Node token not configured")

    if authorization != f"Bearer {expected_token}":
        raise HTTPException(status_code=401, detail="Invalid node token")

    client_ip = request.headers.get("x-forwarded-for") or (
        request.client.host if request.client else None
    )
    update_node_seen(node, x_middlines_version, client_ip)

    firmware = None
    if state["version"] and state["filename"] and state["sha256"]:
        firmware = {
            "version": state["version"],
            "url": f"https://middlines.com/api/node/artifacts/{state['filename']}",
            "sha256": state["sha256"],
        }

    return {
        "node": node,
        "poll_interval_s": state["poll_interval_s"],
        "firmware": firmware,
        "restart_nonce": state["restart_nonce"],
    }


@app.get("/node/artifacts/{filename}")
def get_artifact(filename: str) -> FileResponse:
    artifact_path = ARTIFACTS_DIR / Path(filename).name
    if not artifact_path.is_file():
        raise HTTPException(status_code=404, detail="Artifact not found")
    return FileResponse(artifact_path)


@app.get("/admin/login")
def admin_login_page() -> HTMLResponse:
    return html_page(
        "Admin Login",
        """
        <div class='card' style='max-width: 420px; margin: 80px auto;'>
          <h1>Admin Login</h1>
          <form method='post' action='/api/admin/login'>
            <label>Username<input name='username' value='admin' autocomplete='username'></label>
            <label>Password<input type='password' name='password' autocomplete='current-password'></label>
            <button type='submit'>Sign in</button>
          </form>
        </div>
        """,
    )


@app.post("/admin/login")
def admin_login_submit(
    username: Annotated[str, Form()],
    password: Annotated[str, Form()],
) -> RedirectResponse:
    if username != ADMIN_USERNAME or password != ADMIN_PASSWORD:
        return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/login", status_code=303)
    response = RedirectResponse(f"{PUBLIC_API_PREFIX}/admin", status_code=303)
    response.set_cookie(
        SESSION_COOKIE, sign_session_value(username), httponly=True, samesite="lax"
    )
    return response


@app.post("/admin/logout")
def admin_logout() -> RedirectResponse:
    response = RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/login", status_code=303)
    response.delete_cookie(SESSION_COOKIE)
    return response


@app.get("/admin")
def admin_dashboard(request: Request) -> HTMLResponse:
    require_admin(request)
    nodes, artifacts = fetch_admin_dashboard_data()

    node_rows = "".join(
        f"<tr><td><a href='{PUBLIC_API_PREFIX}/admin/nodes/{escape(row['node'])}'>{escape(row['node'])}</a></td>"
        f"<td>{escape(row['current_version'] or 'unknown')}</td>"
        f"<td>{escape(row['target_version'] or 'none')}</td>"
        f"<td>{escape(row['last_seen_at'] or 'never')}</td></tr>"
        for row in nodes
    )
    artifact_rows = "".join(
        f"<tr><td>{escape(row['version'])}</td><td class='mono'>{escape(row['filename'])}</td>"
        f"<td>{row['size_bytes']}</td><td>{escape(row['uploaded_at'])}</td></tr>"
        for row in artifacts
    )

    content = f"""
    <div class='grid'>
      <div class='card'>
        <h2>Upload Firmware</h2>
        <form method='post' action='/api/admin/firmware/upload' enctype='multipart/form-data'>
          <label>Version<input name='version' placeholder='1.2.3' required></label>
          <label>Binary<input type='file' name='artifact' accept='.bin' required></label>
          <button type='submit'>Upload OTA Binary</button>
        </form>
      </div>
      <div class='card'>
        <h2>Nodes</h2>
        <table>
          <thead><tr><th>Node</th><th>Current</th><th>Target</th><th>Last Seen</th></tr></thead>
          <tbody>{node_rows}</tbody>
        </table>
      </div>
    </div>
    <div class='card'>
      <h2>Uploaded Firmware</h2>
      <table>
        <thead><tr><th>Version</th><th>Stored File</th><th>Bytes</th><th>Uploaded</th></tr></thead>
        <tbody>{artifact_rows or '<tr><td colspan="4">No firmware uploaded yet.</td></tr>'}</tbody>
      </table>
    </div>
    """
    return render_admin_shell("Control Dashboard", content)


@app.get("/admin/nodes/{node}")
def admin_node_detail(request: Request, node: str) -> HTMLResponse:
    require_admin(request)
    detail, artifacts = fetch_node_detail(node)
    artifact_options = "".join(
        f"<option value='{row['id']}' {'selected' if row['id'] == detail['target_firmware_id'] else ''}>{escape(row['version'])} ({escape(row['filename'])})</option>"
        for row in artifacts
    )
    if not artifact_options:
        artifact_options = "<option value=''>No uploaded firmware</option>"

    content = f"""
    <div class='grid'>
      <div class='card'>
        <h2>Node {escape(node)}</h2>
        <p><strong>Current version:</strong> {escape(detail["current_version"] or "unknown")}</p>
        <p><strong>Last seen:</strong> {escape(detail["last_seen_at"] or "never")}</p>
        <p><strong>Last manifest fetch:</strong> {escape(detail["last_manifest_fetch_at"] or "never")}</p>
        <p><strong>Last IP:</strong> {escape(detail["last_ip"] or "unknown")}</p>
        <p><strong>Target firmware:</strong> {escape(detail["target_version"] or "none")}</p>
      </div>
      <div class='card'>
        <h2>Node Auth</h2>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/token'>
          <label>Bearer token<input class='mono' name='token' value='{escape(detail["token"])}'></label>
          <div class='row'>
            <button type='submit'>Save token</button>
          </div>
        </form>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/token/generate' style='margin-top:8px;'>
          <button type='submit' class='secondary'>Generate new token</button>
        </form>
      </div>
      <div class='card'>
        <h2>Polling</h2>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/poll-interval'>
          <label>Poll interval (seconds)<input name='poll_interval_s' type='number' min='30' step='1' value='{detail["poll_interval_s"]}' required></label>
          <button type='submit'>Save poll interval</button>
        </form>
      </div>
      <div class='card'>
        <h2>OTA Target</h2>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/target-firmware'>
          <label>Firmware<select name='firmware_id'>{artifact_options}</select></label>
          <button type='submit'>Set target firmware</button>
        </form>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/target-firmware/clear' style='margin-top:8px;'>
          <button type='submit' class='secondary'>Clear target firmware</button>
        </form>
      </div>
      <div class='card'>
        <h2>Restart</h2>
        <p class='muted'>Current restart nonce: <span class='mono'>{escape(detail["restart_nonce"] or "none")}</span></p>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/restart'>
          <button type='submit'>Trigger remote restart</button>
        </form>
      </div>
    </div>
    """
    return render_admin_shell(f"Node {node}", content)


@app.post("/admin/firmware/upload")
async def admin_upload_firmware(
    request: Request,
    version: Annotated[str, Form()],
    artifact: Annotated[UploadFile, File()],
) -> RedirectResponse:
    require_admin(request)
    cleaned_name = Path(artifact.filename or "firmware.bin").name
    safe_name = cleaned_name.replace(" ", "-")
    stored_name = f"{version}-{safe_name}"
    target_path = ARTIFACTS_DIR / stored_name
    hasher = hashlib.sha256()
    size_bytes = 0

    with target_path.open("wb") as output:
        while True:
            chunk = await artifact.read(1024 * 1024)
            if not chunk:
                break
            output.write(chunk)
            hasher.update(chunk)
            size_bytes += len(chunk)

    db = get_control_db_connection()
    db.execute(
        """
        INSERT INTO firmware_artifacts (filename, original_filename, version, sha256, size_bytes, uploaded_at)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(filename) DO UPDATE SET
            original_filename = excluded.original_filename,
            version = excluded.version,
            sha256 = excluded.sha256,
            size_bytes = excluded.size_bytes,
            uploaded_at = excluded.uploaded_at
        """,
        (stored_name, cleaned_name, version, hasher.hexdigest(), size_bytes, utc_now()),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin", status_code=303)


@app.post("/admin/nodes/{node}/token")
def admin_set_node_token(
    request: Request,
    node: str,
    token: Annotated[str, Form()],
) -> RedirectResponse:
    require_admin(request)
    db = get_control_db_connection()
    db.execute(
        "UPDATE nodes SET token = ?, updated_at = ? WHERE node = ?",
        (token.strip(), utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/token/generate")
def admin_generate_node_token(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    token = secrets.token_urlsafe(24)
    db = get_control_db_connection()
    db.execute(
        "UPDATE nodes SET token = ?, updated_at = ? WHERE node = ?",
        (token, utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/poll-interval")
def admin_set_poll_interval(
    request: Request,
    node: str,
    poll_interval_s: Annotated[int, Form()],
) -> RedirectResponse:
    require_admin(request)
    interval = max(30, poll_interval_s)
    db = get_control_db_connection()
    db.execute(
        "UPDATE nodes SET poll_interval_s = ?, updated_at = ? WHERE node = ?",
        (interval, utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/target-firmware")
def admin_set_target_firmware(
    request: Request,
    node: str,
    firmware_id: Annotated[str, Form()],
) -> RedirectResponse:
    require_admin(request)
    firmware_value = int(firmware_id) if firmware_id else None
    db = get_control_db_connection()
    db.execute(
        "UPDATE node_desired_state SET target_firmware_id = ?, updated_at = ? WHERE node = ?",
        (firmware_value, utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/target-firmware/clear")
def admin_clear_target_firmware(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    db = get_control_db_connection()
    db.execute(
        "UPDATE node_desired_state SET target_firmware_id = NULL, updated_at = ? WHERE node = ?",
        (utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/restart")
def admin_trigger_restart(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    db = get_control_db_connection()
    db.execute(
        "UPDATE node_desired_state SET restart_nonce = ?, updated_at = ? WHERE node = ?",
        (utc_now(), utc_now(), node),
    )
    db.commit()
    db.close()
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)
