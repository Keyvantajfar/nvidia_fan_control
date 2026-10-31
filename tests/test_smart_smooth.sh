#!/usr/bin/env bash
# Smart Smooth: a 69<->70C wobble across a steep cliff must not make the fan
# jump, a real temperature rise must be followed, and ramp-down must be gradual.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

MOCK_DIR="$TMPDIR/mock"
CONFIG_PATH="$TMPDIR/config.conf"
mkdir -p "$MOCK_DIR"
cp "$ROOT_DIR/tests/smart_flap.conf" "$CONFIG_PATH"
export NVML_MOCK_DIR="$MOCK_DIR" NVFC_CONFIG_PATH="$CONFIG_PATH" NVFC_STATUS_PATH="$TMPDIR/status"

writes() { grep -c "speed=" "$MOCK_DIR/fan_speed.log" || true; }

echo 69 >"$MOCK_DIR/temperature"
"$BINARY" >"$TMPDIR/out.log" 2>&1 &
PID=$!
sleep 2

# Phase 1: wobble for 14 seconds.
for _ in $(seq 1 7); do
  echo 70 >"$MOCK_DIR/temperature"; sleep 1
  echo 69 >"$MOCK_DIR/temperature"; sleep 1
done
flap_writes=$(writes)
if [ "$flap_writes" -gt 2 ]; then
  echo "Expected <=2 fan writes during 69/70 wobble, got $flap_writes" >&2
  cat "$MOCK_DIR/fan_speed.log" >&2
  exit 1
fi

# Phase 2: a real jump to 90C bypasses smoothing (emergency) and hits the curve end.
echo 90 >"$MOCK_DIR/temperature"
sleep 3
if ! tail -n 3 "$MOCK_DIR/fan_speed.log" | grep -q "speed=99"; then
  echo "Expected fan to reach 99% at 90C" >&2
  cat "$MOCK_DIR/fan_speed.log" >&2
  exit 1
fi

# Phase 3: cooling to 40C must ramp down in steps (<=5%), not drop in one write.
echo 40 >"$MOCK_DIR/temperature"
sleep 1
before=$(writes)
sleep 1
if [ "$(writes)" -gt "$((before + 1))" ]; then
  echo "Ramp-down should be held back, not immediate" >&2
  exit 1
fi

# Status file should exist while running and be removed on exit.
[ -f "$TMPDIR/status" ] || { echo "status file missing" >&2; exit 1; }
grep -q "^smooth=1" "$TMPDIR/status" || { echo "status file malformed" >&2; exit 1; }

kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
[ ! -f "$TMPDIR/status" ] || { echo "status file not cleaned up" >&2; exit 1; }

# Ramp-down writes (after the 99% write) must each drop by at most 5%.
awk '/speed=/{split($0,a,"speed=");split(a[2],b," ");s=b[1]+0; if(seen99&&s<prev&&prev-s>5){bad=1} if(s==99)seen99=1; prev=s} END{exit bad?1:0}' \
  "$MOCK_DIR/fan_speed.log" || { echo "Ramp-down step too large" >&2; exit 1; }
