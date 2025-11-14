#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia_fan_controlV2d"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

MOCK_DIR="$TMPDIR/mock"
CONFIG_PATH="$TMPDIR/config.conf"
LOG_PATH="$TMPDIR/stdout.log"
mkdir -p "$MOCK_DIR"
cp "$ROOT_DIR/tests/config_bad.conf" "$CONFIG_PATH"

rm -f "$MOCK_DIR/fan_speed.log"

export NVML_MOCK_DIR="$MOCK_DIR"
export NVFC_CONFIG_PATH="$CONFIG_PATH"

"$BINARY" >"$LOG_PATH" 2>&1 &
PID=$!

sleep 6
kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

if ! grep -q "Invalid fan curve" "$LOG_PATH"; then
  echo "Expected invalid curve warning" >&2
  exit 1
fi

if ! grep -q "temps=\[0,40,55,67,75,85\]" "$LOG_PATH"; then
  echo "Expected default temps in startup log" >&2
  exit 1
fi

if ! grep -q "sleep_low=3" "$LOG_PATH" || ! grep -q "sleep_high=1" "$LOG_PATH"; then
  echo "Expected runtime overrides in startup log" >&2
  exit 1
fi

if [ ! -f "$MOCK_DIR/fan_speed.log" ]; then
  echo "mock fan log not created" >&2
  exit 1
fi

if awk '/speed=/{split($0,a,"speed=");split(a[2],b," ");s=b[1]+0;found=1;if(s<0||s>100)exit 1} END{exit(found?0:1)}' "$MOCK_DIR/fan_speed.log" >/dev/null; then
  :
else
  echo "Fan speed out of range" >&2
  exit 1
fi
