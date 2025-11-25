# middlines

Real-time dining hall line tracking for Middlebury College.

## Architecture
```
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│  ESP32 Node     │      │  ESP32 Node     │      │  ESP32 Node     │
│  (Ross)         │      │  (Atwater)      │      │  (Proctor)      │
└────────┬────────┘      └────────┬────────┘      └────────┬────────┘
         │                        │                        │
         │        MQTT: middlines/{location}/count         │
         └────────────────────────┼────────────────────────┘
                                  ▼
                       ┌─────────────────────┐
                       │  Mosquitto (MQTT)   │
                       │  :1883              │
                       └──────────┬──────────┘
                                  │ subscribe
                                  ▼
                       ┌─────────────────────┐
                       │  Ingester (Python)  │
                       │  - Smoothing view   │
                       └──────────┬──────────┘
                                  │ write
                                  ▼
                       ┌──────────────────────────────────┐
                       │  SQLite (/data/middlines.db)     │
                       │  ┌────────────┐  ┌─────────────┐ │
                       │  │ Raw Counts │  │ Aggregator  │ │
                       │  │ (smoothed) │  │   Tables    │ │
                       │  └────────────┘  │ - baselines │ │
                       │                  │ - max_count │ │
                       │                  │ - averages  │ │
                       │                  └─────────────┘ │
                       └───┬──────────────────────────┬───┘
                           │ read/write               │ read
                  ┌────────┘                          └────────┐
                  ▼                                            ▼
       ┌─────────────────────┐                      ┌─────────────────────┐
       │  Aggregator         │                      │  FastAPI            │
       │  (daily 4:15 AM)    │                      │  :8000              │
       │  - Compute baselines│                      └──────────┬──────────┘
       │  - Compute max count│                                 │ /api/*
       │  - Time averages    │                                 ▼
       └─────────────────────┘                      ┌─────────────────────┐
                                                    │  Nginx              │
                                                    │  :80                │
                                                    │  - /api/* → api     │
                                                    │  - /* → static      │
                                                    └──────────┬──────────┘
                                                               │
                                                               ▼
                                                    ┌─────────────────────┐
                                                    │ Vite React Frontend │
                                                    └─────────────────────┘
```

## Development Setup

Install [uv](https://docs.astral.sh/uv/getting-started/installation/), then:
```bash
uv sync --all-packages
pre-commit install
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

## Data Processing

**Ingester Service:**
- Subscribes to MQTT topics and writes raw counts to SQLite
- Creates a `smoothed_counts` view with rolling average to reduce noise
- Seeds database with realistic test data (full month of November 2025 with meal-time patterns)

**Aggregator Service:**
- Runs daily at 4:15 AM to compute derived statistics:
  - **Baselines**: Nighttime (1-3 AM) averages to detect when halls are closed
  - **Max Counts**: 99th percentile counts for each location (baseline-adjusted)
  - **Time Averages**: Average counts by location/day/time bucket for "vs typical" comparisons
- Uses 45-day lookback window for calculations
- Stores results in dedicated tables (`baseline`, `max_count`, `time_averages`)

## Project Structure
```
middlines/
├── services/
│   ├── ingester/    # MQTT → SQLite with data smoothing
│   ├── aggregator/  # Daily statistical aggregation
│   └── api/         # FastAPI data serving backend
├── frontend/        # Nginx + React + Vite
├── mosquitto/       # MQTT broker config
└── data/            # SQLite database (gitignored)
```
