#!/usr/bin/env bash
# ramp_down levels: after a sudden cool-down from 90C to 40C, "quick" must have
# brought the fan well down within ~14s, "balanced" part-way, and "slow" (the
# 1.0.0 behaviour) must still be at/near full speed.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

run_level() {
  local level=$1 dir="$TMPDIR/$1"
  mkdir -p "$dir"
  cat >"$dir/c.conf" <<CONF
[curve]
temps = 0,60,70,85
speeds = 35,50,62,99

[run]
sleep_low = 1
sleep_high = 1
sleep_max = 1
high_temp_threshold = 42

[smooth]
enabled = on
ramp_down = $level
CONF
  echo 90 >"$dir/temperature"
  NVML_MOCK_DIR="$dir" NVFC_CONFIG_PATH="$dir/c.conf" NVFC_STATUS_PATH="$dir/status" \
    "$BINARY" >"$dir/out.log" 2>&1 &
  local pid=$!
  sleep 2
  echo 40 >"$dir/temperature"
  sleep 14
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  grep -q "ramp_down=$level" "$dir/out.log" || { echo "startup log should name level $level" >&2; exit 1; }
  # last fan speed written before shutdown (ignore reset_to_auto)
  grep 'speed=' "$dir/fan_speed.log" | tail -n 1 | sed 's/.*speed=//'
}

quick=$(run_level quick)
balanced=$(run_level balanced)
slow=$(run_level slow)
echo "fan after 14s cool-down: quick=$quick balanced=$balanced slow=$slow"

[ "$quick" -le 55 ]     || { echo "quick should be down near the target (<=55), got $quick" >&2; exit 1; }
[ "$slow" -ge 94 ]      || { echo "slow should barely have moved (>=94), got $slow" >&2; exit 1; }
[ "$quick" -lt "$balanced" ] && [ "$balanced" -lt "$slow" ] || { echo "levels should order quick < balanced < slow" >&2; exit 1; }
