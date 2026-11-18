#!/usr/bin/env bash
# The built-in profiles exist twice (src/config.c for the daemon, tui/nvfc_config.py
# for the UI and CLI). They must say the same thing.
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BINARY="$ROOT_DIR/build/nvidia-fan-controld"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/mock"
echo 40 >"$TMPDIR/mock/temperature"
export NVML_MOCK_DIR="$TMPDIR/mock" NVFC_STATUS_PATH="$TMPDIR/status"

python3 -I - "$ROOT_DIR/tui" >"$TMPDIR/expected" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import nvfc_config as c
for pid in c.BUILTIN_IDS:
    p = c.BUILTINS[pid]
    print(pid, ",".join(map(str, p.temps)), ",".join(map(str, p.speeds)), p.ramp_down)
PY

while read -r id temps speeds ramp; do
  printf '[profile]\nactive = %s\n' "$id" >"$TMPDIR/c.conf"
  NVFC_CONFIG_PATH="$TMPDIR/c.conf" "$BINARY" >"$TMPDIR/log" 2>&1 &
  pid=$!
  sleep 1.2
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  grep -q "temps=\[$temps\], speeds=\[$speeds\]" "$TMPDIR/log" || { echo "$id: curve differs between daemon and python" >&2; exit 1; }
  grep -q "ramp_down=$ramp" "$TMPDIR/log" || { echo "$id: ramp differs between daemon and python" >&2; exit 1; }
done <"$TMPDIR/expected"
echo ok
