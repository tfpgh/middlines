FROM ghcr.io/astral-sh/uv:python3.14-trixie-slim

WORKDIR /db-init

# Copy dependency files first for better caching
COPY pyproject.toml ./

RUN uv sync --no-install-project

COPY main.py ./

RUN uv sync

CMD ["uv", "run", "main.py"]
