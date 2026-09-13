import hashlib
import hmac
import os
import secrets
from base64 import urlsafe_b64decode, urlsafe_b64encode
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from dataclasses import dataclass, replace
from datetime import datetime
from html import escape
from pathlib import Path
from typing import Annotated
from uuid import UUID, uuid4

from dashboard import LOCATIONS, Dashboard, LocationStatus
from fastapi import (
    Body,
    FastAPI,
    File,
    Form,
    Header,
    HTTPException,
    Request,
    UploadFile,
)
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import FileResponse, HTMLResponse, RedirectResponse, Response
from loguru import logger
from store import ClickHouseStore, FirmwareArtifact, NodeCheckin, NodeConfig
from telemetry import ProtocolError, decode_observations

ARTIFACTS_DIR = Path(os.environ.get("MIDDLINES_ARTIFACTS_DIR", "/data/ota"))

ADMIN_USERNAME = os.environ.get("MIDDLINES_ADMIN_USERNAME", "admin")
ADMIN_PASSWORD = os.environ.get("MIDDLINES_ADMIN_PASSWORD", "changeme")
SESSION_SECRET = os.environ.get("MIDDLINES_SESSION_SECRET", "dev-session-secret")
DEFAULT_POLL_INTERVAL_S = 300
SESSION_COOKIE = "middlines_admin"
PUBLIC_API_PREFIX = "/api"

_store: ClickHouseStore | None = None
_dashboard: Dashboard | None = None


@dataclass(frozen=True, slots=True)
class AdminNode:
    node: str
    token: str
    poll_interval_s: int
    current_version: str | None
    last_seen_at: datetime | None
    last_ip: str | None
    restart_nonce: UUID | None
    target_firmware_sha256: str | None
    target_version: str | None


def ensure_directories() -> None:
    ARTIFACTS_DIR.mkdir(parents=True, exist_ok=True)


def get_store() -> ClickHouseStore:
    if _store is None:
        raise RuntimeError("ClickHouse store is not initialized")
    return _store


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


def fetch_admin_dashboard_data() -> tuple[list[AdminNode], list[FirmwareArtifact]]:
    store = get_store()
    artifacts = store.list_firmware_artifacts()
    artifacts_by_sha256 = artifact_lookup(artifacts)
    checkins = {item.node: item for item in store.list_latest_node_checkins()}
    nodes = [
        build_admin_node(config, checkins.get(config.node), artifacts_by_sha256)
        for config in store.list_latest_node_configs()
    ]
    return nodes, artifacts


def fetch_node_detail(node: str) -> tuple[AdminNode, list[FirmwareArtifact]]:
    store = get_store()
    config = store.get_latest_node_config(node)
    if config is None:
        raise HTTPException(status_code=404, detail="Unknown node")

    artifacts = store.list_firmware_artifacts()
    checkins = {item.node: item for item in store.list_latest_node_checkins()}
    return build_admin_node(
        config, checkins.get(node), artifact_lookup(artifacts)
    ), artifacts


def artifact_lookup(
    artifacts: list[FirmwareArtifact],
) -> dict[str, FirmwareArtifact]:
    result: dict[str, FirmwareArtifact] = {}
    for artifact in artifacts:
        result.setdefault(artifact.sha256, artifact)
    return result


def build_admin_node(
    config: NodeConfig,
    checkin: NodeCheckin | None,
    artifacts_by_sha256: dict[str, FirmwareArtifact],
) -> AdminNode:
    target = (
        artifacts_by_sha256.get(config.target_firmware_sha256)
        if config.target_firmware_sha256
        else None
    )
    return AdminNode(
        node=config.node,
        token=config.token,
        poll_interval_s=config.poll_interval_s,
        current_version=(checkin.firmware_version or None) if checkin else None,
        last_seen_at=checkin.seen_at if checkin else None,
        last_ip=(checkin.client_ip or None) if checkin else None,
        restart_nonce=config.restart_nonce,
        target_firmware_sha256=config.target_firmware_sha256,
        target_version=target.version if target else None,
    )


def format_timestamp(value: datetime | None) -> str:
    return value.isoformat(timespec="seconds") if value else "never"


def require_node_config(node: str) -> NodeConfig:
    config = get_store().get_latest_node_config(node)
    if config is None:
        raise HTTPException(status_code=404, detail="Unknown node")
    return config


def authenticate_node(node: str, authorization: str | None) -> NodeConfig:
    config = require_node_config(node)
    if not config.token:
        raise HTTPException(status_code=403, detail="Node token not configured")
    if not hmac.compare_digest(authorization or "", f"Bearer {config.token}"):
        raise HTTPException(status_code=401, detail="Invalid node token")
    return config


def is_sha256(value: str) -> bool:
    return len(value) == 64 and all(
        character in "0123456789abcdef" for character in value
    )


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    global _store, _dashboard

    ensure_directories()
    store = ClickHouseStore.from_env()
    try:
        store.ensure_nodes(LOCATIONS, DEFAULT_POLL_INTERVAL_S)
        _store = store
        _dashboard = Dashboard(store)
        logger.info("API starting with ClickHouse")
        yield
    finally:
        _store = None
        _dashboard = None
        store.close()
        logger.info("API shutting down")


app = FastAPI(lifespan=lifespan, root_path="/api")
app.add_middleware(GZipMiddleware)


@app.get("/health")
def health() -> str:
    return "Ok"


@app.get("/current", tags=["dashboard"])
def get_current(response: Response) -> list[LocationStatus]:
    response.headers["Cache-Control"] = "no-store"
    if _dashboard is None:
        raise HTTPException(status_code=503, detail="Dashboard is not initialized")
    try:
        return _dashboard.current()
    except Exception as exc:
        logger.exception("Failed to load dashboard")
        raise HTTPException(
            status_code=503, detail="Dining hall data temporarily unavailable"
        ) from exc


@app.get("/node/{node}/manifest")
def get_node_manifest(
    request: Request,
    node: str,
    authorization: Annotated[str | None, Header()] = None,
    x_middlines_version: Annotated[str | None, Header()] = None,
) -> dict[str, object | None]:
    store = get_store()
    config = authenticate_node(node, authorization)

    forwarded_for = request.headers.get("x-forwarded-for")
    if forwarded_for:
        client_ip = forwarded_for.partition(",")[0].strip()
    elif request.client:
        client_ip = request.client.host
    else:
        client_ip = ""
    store.record_node_checkin(
        node=node,
        firmware_version=x_middlines_version or "",
        client_ip=client_ip,
    )

    firmware = None
    if config.target_firmware_sha256:
        artifact = store.get_firmware_artifact(config.target_firmware_sha256)
        if artifact:
            firmware = {
                "version": artifact.version,
                "url": f"https://middlines.com/api/node/artifacts/{artifact.filename}",
                "sha256": artifact.sha256,
            }
        else:
            logger.warning(
                f"Node {node} targets missing firmware {config.target_firmware_sha256}"
            )

    return {
        "node": node,
        "poll_interval_s": config.poll_interval_s,
        "firmware": firmware,
        "restart_nonce": str(config.restart_nonce) if config.restart_nonce else None,
    }


@app.post("/node/{node}/observations", status_code=204)
def ingest_observations(
    node: str,
    payload: Annotated[bytes, Body(media_type="application/octet-stream")],
    authorization: Annotated[str | None, Header()] = None,
) -> Response:
    authenticate_node(node, authorization)
    try:
        observations = decode_observations(payload)
    except ProtocolError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error

    try:
        get_store().insert_observations(node, observations)
    except Exception as error:
        logger.exception(
            f"Failed to insert {len(observations)} observations from {node}"
        )
        raise HTTPException(
            status_code=503, detail="Observation insert failed"
        ) from error

    return Response(status_code=204)


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
        f"<tr><td><a href='{PUBLIC_API_PREFIX}/admin/nodes/{escape(row.node)}'>{escape(row.node)}</a></td>"
        f"<td>{escape(row.current_version or 'unknown')}</td>"
        f"<td>{escape(row.target_version or 'none')}</td>"
        f"<td>{escape(format_timestamp(row.last_seen_at))}</td></tr>"
        for row in nodes
    )
    artifact_rows = "".join(
        f"<tr><td>{escape(row.version)}</td><td class='mono'>{escape(row.filename)}</td>"
        f"<td>{row.size_bytes}</td><td>{escape(format_timestamp(row.uploaded_at))}</td></tr>"
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
        f"<option value='{row.sha256}' {'selected' if row.sha256 == detail.target_firmware_sha256 else ''}>{escape(row.version)} ({escape(row.original_filename)})</option>"
        for row in artifacts
    )
    if not artifact_options:
        artifact_options = "<option value=''>No uploaded firmware</option>"

    content = f"""
    <div class='grid'>
      <div class='card'>
        <h2>Node {escape(node)}</h2>
        <p><strong>Current version:</strong> {escape(detail.current_version or "unknown")}</p>
        <p><strong>Last seen:</strong> {escape(format_timestamp(detail.last_seen_at))}</p>
        <p><strong>Last IP:</strong> {escape(detail.last_ip or "unknown")}</p>
        <p><strong>Target firmware:</strong> {escape(detail.target_version or "none")}</p>
      </div>
      <div class='card'>
        <h2>Node Auth</h2>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/token'>
          <label>Bearer token<input class='mono' name='token' value='{escape(detail.token)}'></label>
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
          <label>Poll interval (seconds)<input name='poll_interval_s' type='number' min='30' max='3600' step='1' value='{detail.poll_interval_s}' required></label>
          <button type='submit'>Save poll interval</button>
        </form>
      </div>
      <div class='card'>
        <h2>OTA Target</h2>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/target-firmware'>
          <label>Firmware<select name='firmware_sha256'>{artifact_options}</select></label>
          <button type='submit'>Set target firmware</button>
        </form>
        <form method='post' action='{PUBLIC_API_PREFIX}/admin/nodes/{escape(node)}/target-firmware/clear' style='margin-top:8px;'>
          <button type='submit' class='secondary'>Clear target firmware</button>
        </form>
      </div>
      <div class='card'>
        <h2>Restart</h2>
        <p class='muted'>Current restart nonce: <span class='mono'>{escape(str(detail.restart_nonce) if detail.restart_nonce else "none")}</span></p>
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
    clean_version = version.strip()
    if not clean_version:
        raise HTTPException(status_code=400, detail="Firmware version is required")
    cleaned_name = Path(artifact.filename or "firmware.bin").name
    temporary_path = ARTIFACTS_DIR / f".upload-{uuid4()}.tmp"
    hasher = hashlib.sha256()
    size_bytes = 0

    try:
        with temporary_path.open("xb") as output:
            while chunk := await artifact.read(1024 * 1024):
                output.write(chunk)
                hasher.update(chunk)
                size_bytes += len(chunk)

        sha256 = hasher.hexdigest()
        stored_name = f"{sha256}.bin"
        existing = get_store().get_firmware_artifact(sha256)
        if existing and existing.version != clean_version:
            raise HTTPException(
                status_code=409,
                detail=f"This binary is already registered as firmware {existing.version}",
            )

        temporary_path.replace(ARTIFACTS_DIR / stored_name)
        if not existing:
            get_store().add_firmware_artifact(
                sha256=sha256,
                version=clean_version,
                filename=stored_name,
                original_filename=cleaned_name,
                size_bytes=size_bytes,
            )
        return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin", status_code=303)
    finally:
        temporary_path.unlink(missing_ok=True)


@app.post("/admin/nodes/{node}/token")
def admin_set_node_token(
    request: Request,
    node: str,
    token: Annotated[str, Form()],
) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    get_store().append_node_config(replace(config, token=token.strip()))
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/token/generate")
def admin_generate_node_token(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    get_store().append_node_config(replace(config, token=secrets.token_urlsafe(24)))
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/poll-interval")
def admin_set_poll_interval(
    request: Request,
    node: str,
    poll_interval_s: Annotated[int, Form()],
) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    interval = min(3600, max(30, poll_interval_s))
    get_store().append_node_config(replace(config, poll_interval_s=interval))
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/target-firmware")
def admin_set_target_firmware(
    request: Request,
    node: str,
    firmware_sha256: Annotated[str, Form()],
) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    if not is_sha256(firmware_sha256):
        raise HTTPException(status_code=400, detail="Invalid firmware SHA-256")
    if get_store().get_firmware_artifact(firmware_sha256) is None:
        raise HTTPException(status_code=404, detail="Firmware artifact not found")
    get_store().append_node_config(
        replace(config, target_firmware_sha256=firmware_sha256)
    )
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/target-firmware/clear")
def admin_clear_target_firmware(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    get_store().append_node_config(replace(config, target_firmware_sha256=None))
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)


@app.post("/admin/nodes/{node}/restart")
def admin_trigger_restart(request: Request, node: str) -> RedirectResponse:
    require_admin(request)
    config = require_node_config(node)
    get_store().append_node_config(replace(config, restart_nonce=uuid4()))
    return RedirectResponse(f"{PUBLIC_API_PREFIX}/admin/nodes/{node}", status_code=303)
