#!/usr/bin/env bash
# Config migration: a schema 1 file (everything written before profiles) must
# upgrade to schema 2 as profile "custom1" and make the daemon behave exactly
# as it did before the upgrade.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
CFG="python3 -I $ROOT_DIR/tui/nvfc_config.py"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/mock"
echo 40 >"$TMPDIR/mock/temperature"
export NVML_MOCK_DIR="$TMPDIR/mock" NVFC_STATUS_PATH="$TMPDIR/status"

fail() { echo "FAIL: $*" >&2; exit 1; }

startup_log() {  # <conf> -> the lines that describe behaviour
  NVFC_CONFIG_PATH="$1" "$BINARY" >"$TMPDIR/log" 2>&1 &
  local pid=$!
  sleep 1.2
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  grep -E 'temps=|Smart Smooth' "$TMPDIR/log" | sed -e 's/^.*started/started/' -e 's/^\[[^]]*\] \[[A-Z]*\] //'
}

# 1. a 1.0.2-style config, including the "advanced" smooth keys.
cat >"$TMPDIR/old.conf" <<'CONF'
[curve]
temps = 0,45,62,80
speeds = 22,33,55,97

[run]
gpu_index = 0
sleep_low = 4
sleep_high = 2
sleep_max = 9
high_temp_threshold = 50

[smooth]
enabled = on
ramp_down = slow
deadband = 5
down_hold = 20
emergency_temp = 88
CONF
cp "$TMPDIR/old.conf" "$TMPDIR/new.conf"
before=$(startup_log "$TMPDIR/old.conf")

msg=$($CFG migrate --config "$TMPDIR/new.conf")
echo "$msg"
grep -q 'upgraded schema 1 -> 2' <<<"$msg" || fail "migration message"
grep -q '^config_version = 2' "$TMPDIR/new.conf" || fail "schema marker missing"
grep -q '^active = custom1' "$TMPDIR/new.conf" || fail "old settings must become the active profile"
grep -q '^name = Previous config' "$TMPDIR/new.conf" || fail "custom1 name"
! grep -q '^\[curve\]' "$TMPDIR/new.conf" || fail "[curve] should be gone"
cmp -s "$TMPDIR/old.conf" "$TMPDIR/new.conf.v1.bak" || fail "backup must equal the original"

after=$(startup_log "$TMPDIR/new.conf")
[ "$before" = "$after" ] || { echo "before: $before" >&2; echo "after:  $after" >&2; fail "daemon behaviour changed by migration"; }

# 2. idempotent; backup is kept, not overwritten.
cp "$TMPDIR/new.conf" "$TMPDIR/new.conf.copy"
$CFG migrate --config "$TMPDIR/new.conf" | grep -q 'already up to date' || fail "second run should be a no-op"
cmp -s "$TMPDIR/new.conf" "$TMPDIR/new.conf.copy" || fail "second run modified the file"
cmp -s "$TMPDIR/old.conf" "$TMPDIR/new.conf.v1.bak" || fail "backup was overwritten"

# 3. a schema 1 file with nothing customised -> balanced, no custom1.
printf '[run]\nsleep_low = 3\n' >"$TMPDIR/plain.conf"
$CFG migrate --config "$TMPDIR/plain.conf" >/dev/null
grep -q '^active = balanced' "$TMPDIR/plain.conf" || fail "plain file should become balanced"

# 4. a newer schema is never touched.
printf '[nvfc]\nconfig_version = 99\n[profile]\nactive = quiet\n' >"$TMPDIR/future.conf"
cp "$TMPDIR/future.conf" "$TMPDIR/future.copy"
$CFG migrate --config "$TMPDIR/future.conf" | grep -q 'newer version' || fail "future schema message"
cmp -s "$TMPDIR/future.conf" "$TMPDIR/future.copy" || fail "future schema file was modified"

# 5. missing file: nothing happens, no crash.
$CFG migrate --config "$TMPDIR/nope.conf" | grep -q 'up to date' || fail "missing file"

# 6. helpers: aliases, custom names, name cleaning, set_active on an old file.
python3 -I - "$ROOT_DIR/tui" "$TMPDIR" <<'PY'
import sys, os
sys.path.insert(0, sys.argv[1])
import nvfc_config as c
tmp = sys.argv[2]

s = c.Settings()
s.custom["custom2"].name = "Gaming"
assert c.resolve_profile(s, "COOL") == "performance"
assert c.resolve_profile(s, "gaming") == "custom2"
assert c.resolve_profile(s, "quiet") == "quiet"
assert c.resolve_profile(s, "nope") is None
assert c.clean_name("  a\tb\nc  ", "x") == "a b c"
assert c.clean_name("\n\n", "Custom 1") == "Custom 1"
assert len(c.clean_name("x" * 100, "y")) == c.NAME_MAX

old = os.path.join(tmp, "set.conf")
open(old, "w").write("[curve]\ntemps = 0,40,70\nspeeds = 30,50,90\n[run]\nsleep_low = 2\n")
c.set_active(old, "quiet")
s2, schema = c.load_settings(old)
assert schema == 2 and s2.active == "quiet"
assert s2.custom["custom1"].temps == [0, 40, 70], "old curve must survive in custom1"
assert s2.run["sleep_low"] == 2
assert os.path.exists(old + ".v1.bak")

# names with odd characters survive a round trip
s2.custom["custom2"].name = "100% quiet = nice"
c.save_settings(old, s2)
assert c.load_settings(old)[0].custom["custom2"].name == "100% quiet = nice"
print("python helpers ok")
PY
echo ok
