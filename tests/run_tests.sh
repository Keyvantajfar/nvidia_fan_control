#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export ROOT_DIR

TESTS=(
  test_config_clamp.sh
  test_reload.sh
  test_smoothing.sh
  test_smart_smooth.sh
  test_adaptive_sleep.sh
  test_ramp_levels.sh
  test_profiles.sh
  test_profile_sync.sh
  test_migrate.sh
  test_tui_profiles.sh
  test_update.sh
  test_version.sh
)

for script in "${TESTS[@]}"; do
  echo "[TEST] ${script}"
  bash "${ROOT_DIR}/tests/${script}"
  echo "[PASS] ${script}"
  echo
done