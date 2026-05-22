# Builder stage
FROM python:3.13-slim AS builder

# Install uv binary
COPY --from=ghcr.io/astral-sh/uv:latest /uv /bin/uv

# Set environment variables for uv and Python stability
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    UV_PYTHON_DOWNLOADS=never

WORKDIR /app

# Copy dependency files first to leverage Docker layer caching
COPY pyproject.toml ./

# Install dependencies (will use system python 3.13 because UV_PYTHON_DOWNLOADS=never)
RUN uv sync --frozen --no-cache --no-install-project

# Copy source code and install project
COPY . .
RUN uv sync --frozen --no-cache

# Runtime stage
FROM python:3.13-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PATH="/app/.venv/bin:$PATH"

WORKDIR /app

# Copy the virtual environment and application from the builder
COPY --from=builder /app /app

EXPOSE 8000 5555

# Default command starts the FastAPI server, but can be overridden
CMD ["uvicorn", "worker.main:app", "--host", "0.0.0.0", "--port", "8000"]
