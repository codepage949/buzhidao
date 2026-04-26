#!/usr/bin/env bash
set -euo pipefail

ci_retry() {
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
