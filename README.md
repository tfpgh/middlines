# middlines

Real-time dining hall line tracking for Middlebury College.

## Architecture
```
┌────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐
│   ESP32    │ │   ESP32    │ │   ESP32    │ │ Simulator  │
│   (Ross)   │ │ (Atwater)  │ │ (Proctor)  │ │            │
└─────┬──────┘ └─────┬──────┘ └─────┬──────┘ └─────┬──────┘
      │              │              │              │
      │       MQTT: middlines/{location}/count     │
      └──────────────────────┬─────────────────────┘
                             ▼
                  ┌────────────────────┐
                  │  Mosquitto (MQTT)  │
                  │       :1883        │
                  └─────────┬──────────┘
                            │ subscribe
                            ▼
                  ┌────────────────────┐
                  │ Ingester (Python)  │
                  └─────────┬──────────┘
                            │ write
                            ▼
                  ┌────────────────────┐
                  │       SQLite       │
                  │  ┌──────────────┐  │
                  │  │ Counts Table │  │
                  │  └──────────────┘  │
                  │  ┌──────────────┐  │
                  │  │ Smoothed View│  │
                  │  └──────────────┘  │
                  └─────────┬──────────┘
                            │ read
                            ▼
                  ┌────────────────────┐
                  │      FastAPI       │
                  │       :8000        │
                  └─────────┬──────────┘
                            │ /api/*
                            ▼
                  ┌────────────────────┐
                  │       Nginx        │
                  │        :80         │
                  └─────────┬──────────┘
                            │
                            ▼
                  ┌────────────────────┐
                  │   React Frontend   │
                  └────────────────────┘
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
