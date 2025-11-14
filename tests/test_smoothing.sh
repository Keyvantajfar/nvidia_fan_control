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
cp "$ROOT_DIR/tests/smoothing.conf" "$CONFIG_PATH"

rm -f "$MOCK_DIR/fan_speed.log"

echo 39 >"$MOCK_DIR/temperature"

export NVML_MOCK_DIR="$MOCK_DIR"
export NVFC_CONFIG_PATH="$CONFIG_PATH"

"$BINARY" >"$LOG_PATH" 2>&1 &
PID=$!

sleep 4

echo 41 >"$MOCK_DIR/temperature"
sleep 5

echo 85 >"$MOCK_DIR/temperature"
sleep 4

kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

if ! grep -q "speed=30" "$MOCK_DIR/fan_speed.log"; then
  echo "Expected baseline speed entry" >&2
  exit 1
fi

if grep -q "speed=33" "$MOCK_DIR/fan_speed.log"; then
  echo "Smoothing should have skipped small step" >&2
  exit 1
fi

if ! grep -q "speed=80" "$MOCK_DIR/fan_speed.log"; then
  echo "Expected high speed entry" >&2
  exit 1
fi
