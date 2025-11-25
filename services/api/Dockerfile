FROM ghcr.io/astral-sh/uv:python3.14-trixie-slim

WORKDIR /api

# Copy dependency files first for better caching
COPY pyproject.toml ./

RUN uv sync --no-install-project

COPY main.py ./

RUN uv sync

CMD ["uv", "run", "uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
