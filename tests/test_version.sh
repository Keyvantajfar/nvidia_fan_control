#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
expected=$(cat "$ROOT_DIR/VERSION")
out=$("$ROOT_DIR/build/nvidia-fan-controld" --version)
[ "$out" = "nvidia-fan-controld $expected" ] || { echo "daemon version mismatch: $out" >&2; exit 1; }
out=$(python3 "$ROOT_DIR/bin/nvfc" --version)
[ "$out" = "nvfc $expected" ] || { echo "cli version mismatch: $out" >&2; exit 1; }
