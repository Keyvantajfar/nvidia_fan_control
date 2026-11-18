#!/usr/bin/env bash
# TUI + CLI profile handling, driven through a pseudo-terminal with scripted keys.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
DRIVE="python3 $ROOT_DIR/tests/tui_drive.py"
NVFC="python3 $ROOT_DIR/bin/nvfc"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
C="$TMPDIR/c.conf"
export NVFC_CONFIG_PATH="$C"

if ! python3 -c 'import curses, pty' 2>/dev/null; then echo "skip: no curses/pty"; exit 0; fi
fail() { echo "FAIL: $*" >&2; cat "$C" >&2; exit 1; }
fresh() { printf '[curve]\ntemps = 0,40,70\nspeeds = 30,50,90\n[run]\nsleep_low = 2\n[smooth]\nramp_down = slow\n' >"$C"; rm -f "$C.v1.bak"; }

# 1. opening and quitting writes nothing.
fresh; cp "$C" "$TMPDIR/orig"
$DRIVE "$C" q >/dev/null || true
cmp -s "$C" "$TMPDIR/orig" || fail "quit without saving must not touch the file"

# 2. an old config opens as custom1 and saving keeps it; switching to a built-in saves only the choice.
fresh
$DRIVE "$C" p UP UP UP ENTER s >/dev/null
grep -q '^active = quiet' "$C" || fail "active should be quiet"
grep -q '^temps = 0,40,70' "$C" || fail "old curve must survive in custom1"
grep -q '^ramp_down = slow' "$C" || fail "old ramp_down must survive in custom1"
grep -q 'name = Previous config' "$C" || fail "custom1 name"
! grep -q '^\[quiet\]' "$C" || fail "built-ins must not be written to the file"
[ -f "$C.v1.bak" ] || fail "backup of the old file expected"

# 3. built-ins are read-only: edits on Quiet change nothing.
cp "$C" "$TMPDIR/before"
$DRIVE "$C" RIGHT RIGHT a r SPACE s >/dev/null
cmp -s "$C" "$TMPDIR/before" || fail "editing a built-in profile must not change the file"

# 4. copy Quiet into custom2 (second target), rename it, save: becomes active, custom1 untouched.
$DRIVE "$C" c DOWN ENTER n BS BS BS BS BS BS BS BS G a m i n g ENTER s >/dev/null
grep -q '^active = custom2' "$C" || fail "copy should switch to custom2"
grep -q '^name = Gaming' "$C" || fail "custom2 name"
awk '/^\[custom2\]/{f=1} f&&/^temps/{print; exit}' "$C" | grep -q '0,45,60,72,82,92,100' || fail "custom2 should hold Quiet's curve"
awk '/^\[custom1\]/{f=1} f&&/^temps/{print; exit}' "$C" | grep -q '^temps = 0,40,70$' || fail "custom1 must be untouched"

# 5. editing a custom profile works and is saved into that slot only.
$DRIVE "$C" RIGHT RIGHT s >/dev/null
awk '/^\[custom2\]/{f=1} f&&/^temps/{print; exit}' "$C" | grep -q '^temps = 2,45,60' || fail "custom2 edit not saved"
grep -q '^temps = 0,40,70$' "$C" || fail "custom1 changed by editing custom2"

# 6. CLI: list works for everyone; switching needs root (it edits /etc) so only run it as root.
$NVFC profile | grep -q '^ \* custom2 .*Gaming' || fail "nvfc profile should mark custom2 active"
if [ "$(id -u)" -eq 0 ]; then
  $NVFC profile cool >/dev/null 2>&1 || true
  grep -q '^active = performance' "$C" || fail "'cool' alias"
  $NVFC profile gaming >/dev/null 2>&1 || true
  grep -q '^active = custom2' "$C" || fail "switch by custom name"
  ! $NVFC profile bogus >/dev/null 2>&1 || fail "unknown profile should fail"
fi
echo ok
