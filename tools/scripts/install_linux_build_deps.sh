#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get is required for this script" >&2
  exit 1
fi

retry() {
  local max_attempts="${CI_RETRY_COUNT:-3}"
  local delay_seconds="${CI_RETRY_DELAY_SECONDS:-5}"
  local attempt=1

  while true; do
    if "$@"; then
      return 0
    fi

    if (( attempt >= max_attempts )); then
      echo "command failed after ${attempt}/${max_attempts} attempts: $*" >&2
      return 1
    fi

    echo "retrying command (${attempt}/${max_attempts}): $*" >&2
    sleep "$delay_seconds"
    attempt=$((attempt + 1))
  done
}

retry sudo apt-get -o Acquire::Retries=5 update
retry sudo apt-get -o Acquire::Retries=5 install -y \
  libwebkit2gtk-4.1-dev \
  libgtk-3-dev \
  libayatana-appindicator3-dev \
  librsvg2-dev \
  patchelf \
  libpipewire-0.3-dev \
  libxdo-dev \
  libx11-dev \
  libxtst-dev \
  libxi-dev \
  libxrandr-dev \
  libxinerama-dev \
  libxcursor-dev \
  libxfixes-dev \
  libevdev-dev \
  libclang-dev \
  libgbm-dev \
  libssl-dev \
  libgl1 \
  libgeos-dev \
  libgomp1 \
  libglib2.0-0 \
  libopencv-dev
