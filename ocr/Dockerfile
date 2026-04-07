FROM python:3.13-slim

WORKDIR /app

ARG PADDLE_RUNTIME=cpu

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 libgeos-dev libgomp1 libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=ghcr.io/astral-sh/uv:latest /uv /bin/uv

COPY pyproject.toml pyproject.gpu.toml ./
RUN if [ "$PADDLE_RUNTIME" = "gpu" ]; then cp pyproject.gpu.toml pyproject.toml.selected; else cp pyproject.toml pyproject.toml.selected; fi \
    && mv pyproject.toml.selected pyproject.toml \
    && uv sync --no-dev

COPY ./ ./

CMD ["uv", "run", "python", "main.py"]
