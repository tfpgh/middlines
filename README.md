# middlines

Real-time dining hall line tracking for Middlebury College.

## Architecture

The repository is currently between data-plane implementations. The retained
services provide the web application, node control/OTA, and MQTT broker:

```
┌────────────┐ ┌────────────┐ ┌────────────┐
│   ESP32    │ │   ESP32    │ │   ESP32    │
│   (ross)   │ │ (atwater)  │ │ (proctor)  │
└─────┬──────┘ └─────┬──────┘ └─────┬──────┘
      │              │              │
      │  HTTPS poll: /api/node/{node}/manifest
      └──────────────────────┬─────────────────────┘
                             ▼
                  ┌────────────────────┐
                  │      FastAPI       │
                  │  node control UI   │
                  │  OTA upload/store  │
                  └─────────┬──────────┘
                            │ /api/*
                            ▼
                   ┌────────────────────┐
                   │   React Frontend   │
                   └────────────────────┘

Planned data plane:

ESP32 -> MQTT -> telemetry ingester -> ClickHouse -> FastAPI `/api/current`
```

The dashboard data endpoint is intentionally absent until its ClickHouse-backed
replacement is implemented. Node control and OTA continue to use the small
`device_control.db` SQLite database managed directly by the API.

## Development Setup

Install [uv](https://docs.astral.sh/uv/getting-started/installation/), then:
```bash
uv sync --all-packages
uv run pre-commit install
```

## Running Locally
```bash
docker compose up --build
```
If you want it to run in the background, add the `-d` flag.

- Frontend: http://localhost:80
- API: http://localhost:80/api (proxied through Nginx)
- Admin: http://localhost:80/api/admin
- MQTT: localhost:1883

## Testing MQTT

```bash
# Subscribe to all topics
docker exec mosquitto mosquitto_sub -t "middlines/#" -v
```

## Services

**Mosquitto:**
- MQTT broker retained for the new telemetry data plane

**API:**
- Hosts the node control plane:
  - `/api/node/{node}/manifest`
  - `/api/node/artifacts/{filename}`
  - `/api/admin` for OTA uploads, restart requests, and node token management
- Initializes and uses `device_control.db` for control state

**Frontend and Nginx:**
- Serve the React application and proxy `/api/*` to FastAPI

## Hardware Provisioning

The transition firmware uses one NVS namespace:

- `control`
  - `node` - node identifier (`ross`, `proctor`, or `atwater`)
  - `url` - base FastAPI URL, for example `https://middlines.com/api`
  - `token` - bearer token assigned per node from `/api/admin`

MQTT-specific provisioning will be added with the new telemetry transport.

## Project Structure
```
middlines/
├── services/
│   └── api/         # Control and OTA API
├── frontend/        # Nginx + React + Vite
├── mosquitto/       # MQTT broker config
├── nginx-proxy/     # Public TLS proxy
├── hardware/        # ESP32 firmware
└── data/            # Control database and OTA artifacts (gitignored)
```
