#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT_DIR/tools/scripts/ci_retry.sh"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

attempt_file="$tmp_dir/attempts"

CI_RETRY_COUNT=3 CI_RETRY_DELAY_SECONDS=0 ci_retry bash -c '
  attempts_file="$1"
  attempts=0
  if [ -f "$attempts_file" ]; then
    attempts="$(cat "$attempts_file")"
  fi
  attempts=$((attempts + 1))
  echo "$attempts" > "$attempts_file"
  [ "$attempts" -ge 2 ]
' _ "$attempt_file"

if [ "$(cat "$attempt_file")" != "2" ]; then
  echo "retry count mismatch" >&2
  exit 1
fi

echo "ci_retry test ok"
