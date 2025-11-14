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
cp "$ROOT_DIR/tests/reload_initial.conf" "$CONFIG_PATH"

rm -f "$MOCK_DIR/fan_speed.log"

export NVML_MOCK_DIR="$MOCK_DIR"
export NVFC_CONFIG_PATH="$CONFIG_PATH"

"$BINARY" >"$LOG_PATH" 2>&1 &
PID=$!

sleep 3

echo 82 >"$MOCK_DIR/temperature"
sleep 5

cp "$ROOT_DIR/tests/reload_new.conf" "$CONFIG_PATH"
kill -HUP "$PID"

sleep 2
echo 50 >"$MOCK_DIR/temperature"
sleep 3
echo 84 >"$MOCK_DIR/temperature"
sleep 5

kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

if ! grep -q "Reloaded config" "$LOG_PATH"; then
  echo "Expected reload log entry" >&2
  exit 1
fi

if ! grep -q "speed=78" "$MOCK_DIR/fan_speed.log"; then
  echo "Expected initial speed entry" >&2
  exit 1
fi

if ! grep -q "speed=90" "$MOCK_DIR/fan_speed.log"; then
  echo "Expected updated speed entry after reload" >&2
  exit 1
fi
