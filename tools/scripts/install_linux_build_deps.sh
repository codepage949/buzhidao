#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "apt-get is required for this script" >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
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
