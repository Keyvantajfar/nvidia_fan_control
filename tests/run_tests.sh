#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export ROOT_DIR

TESTS=(
  test_config_clamp.sh
  test_reload.sh
  test_smoothing.sh
)

for script in "${TESTS[@]}"; do
  echo "[TEST] ${script}"
  "${ROOT_DIR}/tests/${script}"
  echo "[PASS] ${script}"
  echo
done
