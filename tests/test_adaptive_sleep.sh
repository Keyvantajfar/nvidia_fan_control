#!/usr/bin/env bash
# Adaptive polling: when cool and stable the interval grows (fewer wake-ups), and
# a temperature rise snaps it straight back to the fast interval.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

MOCK_DIR="$TMPDIR/mock"
CONFIG_PATH="$TMPDIR/config.conf"
STATUS="$TMPDIR/status"
mkdir -p "$MOCK_DIR"
cat >"$CONFIG_PATH" <<'CONF'
[curve]
temps = 0,40,60,80
speeds = 25,35,55,90

[run]
sleep_low = 1
sleep_high = 1
sleep_max = 4
high_temp_threshold = 50
CONF
export NVML_MOCK_DIR="$MOCK_DIR" NVFC_CONFIG_PATH="$CONFIG_PATH" NVFC_STATUS_PATH="$STATUS"

interval() { sed -n 's/^interval=//p' "$STATUS"; }

echo 35 >"$MOCK_DIR/temperature"
"$BINARY" >"$TMPDIR/out.log" 2>&1 &
PID=$!
sleep 12   # 3 quiet polls at 1s, then 2s, 4s ... capped at 4
if [ "$(interval)" != "4" ]; then
  echo "Expected backoff to reach sleep_max=4, got '$(interval)'" >&2
  exit 1
fi

# Heat spike: next poll must return to the base interval immediately.
echo 55 >"$MOCK_DIR/temperature"
sleep 5
if [ "$(interval)" != "1" ]; then
  echo "Expected interval to snap back to 1 on rise, got '$(interval)'" >&2
  exit 1
fi

kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
