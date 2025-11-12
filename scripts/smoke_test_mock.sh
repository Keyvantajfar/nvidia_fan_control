#!/bin/sh
# Smoke test for the mock NVML backend.
# Builds the daemon against mock NVML, drives temperature changes, and verifies
# that requested fan speeds increase with temperature.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR="$SCRIPT_DIR/.."
BUILD_ROOT="$REPO_DIR/build"
BUILD_DIR="$BUILD_ROOT/mock_smoke"
CONFIG_PATH="$BUILD_DIR/mock.conf"
MOCK_STATE="$BUILD_DIR/mock_state"
LOG_PATH="$MOCK_STATE/fan_speed.log"

mkdir -p "$BUILD_DIR" "$MOCK_STATE"
rm -f "$LOG_PATH"

cat > "$CONFIG_PATH" <<'CFG'
[curve]
temps = 0,40,60,80
speeds = 20,30,60,90

[run]
gpu_index = 0
sleep_low = 1
sleep_high = 1
high_temp_threshold = 50
CFG

python3 "$REPO_DIR/scripts/gen_header_from_conf.py" --config "$CONFIG_PATH" --output "$BUILD_ROOT/fan_curve_config.h"
python3 "$REPO_DIR/scripts/patch_source_for_build.py" --source "$REPO_DIR/nvidia_fan_controlV2.c" --output "$BUILD_ROOT/nvidia_fan_controlV2.c" --header "$BUILD_ROOT/fan_curve_config.h"

make -C "$REPO_DIR/mock_nvml" >/dev/null

gcc -O2 "$BUILD_ROOT/nvidia_fan_controlV2.c" -o "$BUILD_ROOT/nvidia_fan_controlV2d" -I"$REPO_DIR/mock_nvml" -L"$REPO_DIR/mock_nvml" -lnvidia-ml -Wl,-rpath,\$ORIGIN/mock_nvml

export NVML_MOCK_DIR="$MOCK_STATE"
export USE_MOCK_NVML=1

export LD_LIBRARY_PATH="$REPO_DIR/mock_nvml"

timeout 12s "$BUILD_ROOT/nvidia_fan_controlV2d" > "$BUILD_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!

sleep 2
NVML_MOCK_DIR="$MOCK_STATE" "$REPO_DIR/mock_nvml/mockctl.sh" set-temp 40
sleep 2
NVML_MOCK_DIR="$MOCK_STATE" "$REPO_DIR/mock_nvml/mockctl.sh" set-temp 60
sleep 2
NVML_MOCK_DIR="$MOCK_STATE" "$REPO_DIR/mock_nvml/mockctl.sh" set-temp 80

wait $DAEMON_PID || true

if [ ! -f "$LOG_PATH" ]; then
    echo "Smoke test failed: fan_speed.log not produced" >&2
    exit 1
fi

SPEEDS=$(awk -F'speed=' '/speed=/{print $2}' "$LOG_PATH" | awk '{print $1}' | tail -n 3)
if [ -z "$SPEEDS" ]; then
    echo "Smoke test failed: no speed entries recorded" >&2
    exit 1
fi
PREV=-1
for SPEED in $SPEEDS; do
    if [ "$PREV" -ge "$SPEED" ]; then
        echo "Smoke test failed: speeds not increasing" >&2
        exit 1
    fi
    PREV=$SPEED
done

echo "Mock smoke test passed."
