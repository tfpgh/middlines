# middlines

Real-time dining hall line tracking for Middlebury College.

## Architecture
```
┌────────────┐ ┌────────────┐ ┌────────────┐
│   ESP32    │ │   ESP32    │ │   ESP32    │
│   (ross)   │ │ (atwater)  │ │ (proctor)  │
└─────┬──────┘ └─────┬──────┘ └─────┬──────┘
      │              │              │
      │  HTTPS poll: /api/node/{node}/manifest
      │  HTTPS write: Influx device_logs + advertisements
      └──────────────────────┬─────────────────────┘
                             ▼
                  ┌────────────────────┐
                  │      FastAPI       │
                  │  node control UI   │
                  │  OTA upload/store  │
                  └─────────┬──────────┘
                            │ /api/*
            ┌───────────────┴───────────────┐
            ▼                               ▼
   ┌────────────────────┐         ┌────────────────────┐
   │   React Frontend   │         │      InfluxDB      │
   │ current public app │         │  ops + adv ingest  │
   └────────────────────┘         └────────────────────┘

Legacy development path still present during the data-plane migration:

ESP32/Simulator -> MQTT -> Ingester -> SQLite -> FastAPI `/api/current`
```

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

# Publish a test count
docker exec mosquitto mosquitto_pub -t "middlines/ross/count" -m "42"
```

## Services

**db-init:**
- Initializes SQLite schema (counts table, smoothed_counts view)
- Enables WAL mode for concurrent read/write

**Ingester:**
- Subscribes to MQTT topics (`middlines/+/count`)
- Writes raw counts to SQLite

**Simulator:**
- Seeds 30 days of historical test data on startup
- Publishes simulated counts every 60 seconds via MQTT

**API:**
- Computes statistics in-memory on each request (cached 30s):
  - Baselines from 1-4 AM readings
  - Max counts (99th percentile, baseline-adjusted)
  - Time averages by day/time bucket for "vs typical"
- Returns busyness percentage, trend, and vs-typical comparison
- Hosts the node control plane:
  - `/api/node/{node}/manifest`
  - `/api/node/artifacts/{filename}`
  - `/api/admin` for OTA uploads, restart requests, and node token management

## Hardware Provisioning

The hardware now uses two NVS namespaces:

- `influx`
  - `node`
  - `url`
  - `db`
  - `token`
- `control`
  - `url` - base FastAPI URL, for example `https://middlines.com/api`
  - `token` - bearer token assigned per node from `/api/admin`

## Project Structure
```
middlines/
├── services/
│   ├── db-init/     # Database initialization
│   ├── ingester/    # MQTT → SQLite
│   ├── simulator/   # Test data generation
│   └── api/         # FastAPI backend
├── frontend/        # Nginx + React + Vite
├── mosquitto/       # MQTT broker config
└── data/            # SQLite database (gitignored)
```
