#!/usr/bin/env bash
# Profiles: built-ins, custom slots, schema 1 (legacy) configs read as "custom1",
# unknown profile fallback, and switching with a reload (SIGHUP).
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
TMPDIR=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$TMPDIR"' EXIT

mkdir -p "$TMPDIR/mock"
echo 40 >"$TMPDIR/mock/temperature"
export NVML_MOCK_DIR="$TMPDIR/mock"
export NVFC_STATUS_PATH="$TMPDIR/status"

fail() { echo "FAIL: $*" >&2; [ -f "${LOG:-}" ] && sed 's/^/  | /' "$LOG" >&2; exit 1; }

# run_daemon <conf> : start, wait for the startup log, stop.
run_daemon() {
  LOG="$TMPDIR/out.log"
  NVFC_CONFIG_PATH="$1" "$BINARY" >"$LOG" 2>&1 &
  PID=$!
  sleep 1.5
  kill -TERM "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
}

# 1. schema 1 file (no [profile]) behaves as before and is reported as custom1.
cat >"$TMPDIR/legacy.conf" <<'CONF'
[curve]
temps = 0,50,70,90
speeds = 20,40,60,100

[run]
sleep_low = 3

[smooth]
enabled = on
ramp_down = slow
CONF
run_daemon "$TMPDIR/legacy.conf"
grep -q 'Profile: custom1 (Previous config)' "$LOG" || fail "legacy file should run as custom1"
grep -q 'temps=\[0,50,70,90\]' "$LOG" || fail "legacy curve not applied"
grep -q 'ramp_down=slow' "$LOG" || fail "legacy ramp_down not applied"

# 2. built-ins need no curve in the file.
for spec in "quiet:0,45,60,72,82,92,100:quick" "balanced:0,40,55,67,75,85:balanced" \
            "performance:0,35,45,55,65,75,85:slow"; do
  IFS=: read -r id temps ramp <<<"$spec"
  printf '[nvfc]\nconfig_version = 2\n[profile]\nactive = %s\n[run]\nsleep_low = 3\n' "$id" >"$TMPDIR/b.conf"
  run_daemon "$TMPDIR/b.conf"
  grep -q "Profile: $id " "$LOG" || fail "$id: profile not reported"
  grep -q "temps=\[$temps\]" "$LOG" || fail "$id: wrong curve"
  grep -q "ramp_down=$ramp" "$LOG" || fail "$id: wrong ramp level"
  grep -q 'sleep_low=3' "$LOG" || fail "$id: [run] must stay global"
done

# 3. custom slots: own name, own curve, own smooth settings; the other slot is ignored.
cat >"$TMPDIR/custom.conf" <<'CONF'
[nvfc]
config_version = 2
[profile]
active = custom2
[run]
sleep_low = 4
[custom1]
name = Office
temps = 0,40,60,80
speeds = 10,20,30,40
[custom2]
name = Gaming
temps = 0,30,60,90
speeds = 50,60,80,100
enabled = off
CONF
run_daemon "$TMPDIR/custom.conf"
grep -q 'Profile: custom2 (Gaming)' "$LOG" || fail "custom2 name"
grep -q 'temps=\[0,30,60,90\]' "$LOG" || fail "custom2 curve"
grep -q 'Smart Smooth: off' "$LOG" || fail "custom2 enabled=off"

# 4. custom slot with no curve falls back to balanced quietly.
printf '[profile]\nactive = custom1\n[custom1]\nname = Empty\n' >"$TMPDIR/empty.conf"
run_daemon "$TMPDIR/empty.conf"
grep -q 'Profile: custom1 (Empty)' "$LOG" || fail "empty custom name"
grep -q 'temps=\[0,40,55,67,75,85\]' "$LOG" || fail "empty custom should use balanced curve"
! grep -q 'Invalid fan curve' "$LOG" || fail "empty custom should not warn"

# 5. unknown profile falls back to balanced with a warning.
printf '[profile]\nactive = turbo\n' >"$TMPDIR/unk.conf"
run_daemon "$TMPDIR/unk.conf"
grep -q "Unknown profile 'turbo'" "$LOG" || fail "unknown profile warning"
grep -q 'Profile: balanced' "$LOG" || fail "unknown profile should become balanced"

# 6. switching with SIGHUP changes profile and the status file follows.
printf '[profile]\nactive = quiet\n[run]\nsleep_low = 1\nsleep_high = 1\nsleep_max = 1\n' >"$TMPDIR/sw.conf"
LOG="$TMPDIR/out.log"
NVFC_CONFIG_PATH="$TMPDIR/sw.conf" "$BINARY" >"$LOG" 2>&1 &
PID=$!
sleep 2
grep -q '^profile=quiet$' "$NVFC_STATUS_PATH" || fail "status should show quiet"
printf '[profile]\nactive = performance\n[run]\nsleep_low = 1\nsleep_high = 1\nsleep_max = 1\n' >"$TMPDIR/sw.conf"
kill -HUP "$PID"
sleep 2.5
kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
grep -q 'Reloaded config' "$LOG" || fail "reload not logged"
grep -q 'Profile: performance' "$LOG" || fail "reload should switch to performance"
echo "ok"
