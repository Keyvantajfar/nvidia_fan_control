#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

SESSION_DIR=$(mktemp -d "${TMPDIR:-/tmp}/nvfc-drill.XXXXXX")
CONFIG_PATH="$SESSION_DIR/nvfc.conf"
MOCK_DIR="$SESSION_DIR/mock"
LOG_PATH="$SESSION_DIR/daemon.log"
ENV_PATH="$SESSION_DIR/env.sh"

mkdir -p "$MOCK_DIR"
cp "$ROOT_DIR/tests/reload_initial.conf" "$CONFIG_PATH"

{
  printf 'export TMPDIR=%q\n' "$SESSION_DIR"
  printf 'export NVML_MOCK_DIR=%q\n' "$MOCK_DIR"
  printf 'export CONFIG_PATH=%q\n' "$CONFIG_PATH"
  printf 'export NVFC_CONFIG_PATH=%q\n' "$CONFIG_PATH"
} >"$ENV_PATH"

pushd "$ROOT_DIR" >/dev/null
USE_MOCK_NVML=1 make build
nohup env NVML_MOCK_DIR="$MOCK_DIR" NVFC_CONFIG_PATH="$CONFIG_PATH" \
  ./build/nvidia_fan_controlV2d >"$LOG_PATH" 2>&1 &
DAEMON_PID=$!
popd >/dev/null

printf 'export DAEMON_PID=%q\n' "$DAEMON_PID" >>"$ENV_PATH"

cat <<MESSAGE
################################################################################
# NVIDIA Fan Control V2 – Mock Reload Drill                                    #
################################################################################

Scratch directory : $SESSION_DIR
Config path       : $CONFIG_PATH
Mock NVML dir     : $MOCK_DIR
Daemon PID        : $DAEMON_PID
Daemon log        : $LOG_PATH
Helper env file   : $ENV_PATH

To join the session in another shell:
  source "$ENV_PATH"

Then run the TUI against the config, edit, and save:
  python3 $ROOT_DIR/tui/nvfc_tui.py --config "\$CONFIG_PATH"

Drive the mock backend while editing:
  $ROOT_DIR/mock_nvml/mockctl.sh set-temp 55
  $ROOT_DIR/mock_nvml/mockctl.sh tail-log

Apply the new curve without rebuilding:
  kill -HUP "\$DAEMON_PID"

Inspect the daemon output:
  tail -f "$LOG_PATH"

When finished, stop the daemon and clean up:
  kill "\$DAEMON_PID"
  rm -rf "$SESSION_DIR"

Enjoy the reload drill!
MESSAGE
