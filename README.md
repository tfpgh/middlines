# middlines

Real-time dining hall line tracking for Middlebury.

## Architecture
```
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│  ESP32 Node     │      │  ESP32 Node     │      │  ESP32 Node     │
│  (Ross)         │      │  (Atwater)      │      │  (Proctor)      │
└────────┬────────┘      └────────┬────────┘      └────────┬────────┘
         │                        │                        │
         │ MQTT: middlines/{location}/count                │
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
                       │                     │
                       └──────────┬──────────┘
                                  │ write
                                  ▼
                       ┌─────────────────────┐
                       │  SQLite             │
                       │  /data/middlines.db │
                       └──────────┬──────────┘
                                  │ read
                                  ▼
                       ┌─────────────────────┐
                       │  FastAPI            │
                       │  :8000              │
                       └──────────┬──────────┘
                                  │ /api/*
                                  ▼
                       ┌─────────────────────┐
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

## Project Structure
```
middlines/
├── services/
│   ├── ingester/    # MQTT → SQLite
│   └── api/         # FastAPI data serving backend
├── frontend/        # Nginx + React + Vite
├── mosquitto/       # MQTT broker config
└── data/            # SQLite database (gitignored)
```
