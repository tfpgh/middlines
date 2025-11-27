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
                  ┌────────────────────────────────┐
                  │  SQLite (/data/middlines.db)   │
                  │  ┌──────────┐  ┌────────────┐  │
                  │  │  Counts  │  │ Aggregator │  │
                  │  │  Table   │  │   Tables   │  │
                  │  └──────────┘  └────────────┘  │
                  │  ┌──────────┐                  │
                  │  │ Smoothed │                  │
                  │  │   View   │                  │
                  │  └──────────┘                  │
                  └───────┬───────────────┬────────┘
                          │ read/write    │ read
                 ┌────────┘               └────────┐
                 ▼                                 ▼
      ┌────────────────────┐            ┌────────────────────┐
      │    Aggregator      │            │      FastAPI       │
      │  (every minute)    │            │       :8000        │
      └────────────────────┘            └─────────┬──────────┘
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

**db-init Service:**
- Initializes the SQLite database schema on startup
- Creates the `counts` table with location/timestamp index
- Creates the `smoothed_counts` view with a 5-row rolling average
- Creates aggregation tables (`baseline`, `max_count`, `time_averages`)
- Enables WAL mode for concurrent read/write performance

**Ingester Service:**
- Subscribes to MQTT topics (`middlines/+/count`)
- Writes raw counts to SQLite with timestamps

**Simulator Service:**
- Seeds 30 days of historical test data on startup
- Generates realistic meal-time traffic patterns (weekday vs weekend)
- Publishes simulated counts every 60 seconds via MQTT

**Aggregator Service:**
- Runs every minute to compute derived statistics:
  - **Baselines**: Nighttime (1-3 AM) averages to detect when halls are closed
  - **Max Counts**: 99th percentile counts for each location (baseline-adjusted)
  - **Time Averages**: Average counts by location/day/time bucket for "vs typical" comparisons
- Uses 45-day lookback window for calculations
- Stores results in dedicated tables (`baseline`, `max_count`, `time_averages`)

**API Service:**
- FastAPI backend serving location status data
- Calculates busyness percentage, vs-typical comparison, and trend direction
- Includes 30-second response caching and gzip compression

## Project Structure
```
middlines/
├── services/
│   ├── db-init/     # Database schema initialization
│   ├── ingester/    # MQTT → SQLite
│   ├── simulator/   # Test data generation
│   ├── aggregator/  # Statistical aggregation (every minute)
│   └── api/         # FastAPI backend
├── frontend/        # Nginx + React + Vite
├── mosquitto/       # MQTT broker config
└── data/            # SQLite database (gitignored)
```
