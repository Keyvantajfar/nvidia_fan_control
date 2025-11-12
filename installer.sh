#!/bin/sh
# Installer for the NVIDIA Fan Control V2 tooling.
#
# This script launches the TUI configurator (if needed), generates the build
# header, compiles the daemon (with optional mock NVML support) and installs the
# daemon, CLI wrapper, helper scripts, and systemd service.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR="$SCRIPT_DIR"
CONFIG_PATH="/etc/nvidia-fan-controlV2.conf"
BUILD_DIR="$REPO_DIR/build"
HEADER_PATH="$BUILD_DIR/fan_curve_config.h"
PATCHED_SOURCE="$BUILD_DIR/nvidia_fan_controlV2.c"
BINARY_PATH="$BUILD_DIR/nvidia_fan_controlV2d"
LIBEXEC_DIR="/usr/local/lib/nvidia-fan-control"

if [ "${NVFC_INSTALLER_ROOT:-0}" -ne 1 ] && [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        echo "[installer] Elevating privileges with sudo..."
        exec sudo NVFC_INSTALLER_ROOT=1 USE_MOCK_NVML="${USE_MOCK_NVML:-}" "$0" "$@"
    else
        echo "[installer] Please run this script as root." >&2
        exit 1
    fi
fi

mkdir -p "$BUILD_DIR"
mkdir -p "$LIBEXEC_DIR"

if [ ! -f "$CONFIG_PATH" ]; then
    echo "[installer] Launching TUI configurator to create $CONFIG_PATH"
    python3 "$REPO_DIR/tui/nvfc_tui.py" --config "$CONFIG_PATH"
fi

python3 "$REPO_DIR/scripts/gen_header_from_conf.py" --config "$CONFIG_PATH" --output "$HEADER_PATH"
python3 "$REPO_DIR/scripts/patch_source_for_build.py" --source "$REPO_DIR/nvidia_fan_controlV2.c" --output "$PATCHED_SOURCE" --header "$HEADER_PATH"

USE_MOCK="${USE_MOCK_NVML:-0}"
if [ "$USE_MOCK" = "1" ]; then
    echo "[installer] Building with mock NVML support"
    (cd "$REPO_DIR/mock_nvml" && make)
    if [ ! -f "$REPO_DIR/mock_nvml/libnvidia-ml.so" ]; then
        echo "[installer] mock_nvml/libnvidia-ml.so not found" >&2
        exit 1
    fi
    gcc -O2 "$PATCHED_SOURCE" -o "$BINARY_PATH" -Imock_nvml -Lmock_nvml -lnvidia-ml -Wl,-rpath,'$ORIGIN/mock_nvml'
else
    gcc -O2 "$PATCHED_SOURCE" -o "$BINARY_PATH" -lnvidia-ml
fi

install -m 755 "$BINARY_PATH" /usr/local/sbin/nvidia_fan_controlV2d
install -m 755 "$REPO_DIR/bin/nvidia_fan_controlV2" /usr/local/bin/nvidia_fan_controlV2
install -m 644 "$REPO_DIR/systemd/nvidia-fan-controlV2.service" /etc/systemd/system/nvidia-fan-controlV2.service

install -m 644 "$REPO_DIR/nvidia_fan_controlV2.c" "$LIBEXEC_DIR/nvidia_fan_controlV2.c"
install -m 755 "$REPO_DIR/tui/nvfc_tui.py" "$LIBEXEC_DIR/nvfc_tui.py"
install -m 755 "$REPO_DIR/scripts/gen_header_from_conf.py" "$LIBEXEC_DIR/gen_header_from_conf.py"
install -m 755 "$REPO_DIR/scripts/patch_source_for_build.py" "$LIBEXEC_DIR/patch_source_for_build.py"
if [ -d "$REPO_DIR/mock_nvml" ]; then
    rm -rf "$LIBEXEC_DIR/mock_nvml"
    cp -R "$REPO_DIR/mock_nvml" "$LIBEXEC_DIR/mock_nvml"
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
else
    echo "[installer] systemctl not available; skipping daemon-reload" >&2
fi

echo "[installer] Installation complete."
if [ "$USE_MOCK" != "1" ] && [ -f "$REPO_DIR/mock_nvml/libnvidia-ml.so" ]; then
    echo "[installer] Tip: export USE_MOCK_NVML=1 to build against the bundled mock NVML library for testing."
fi

echo "Enable and start with: systemctl enable --now nvidia-fan-controlV2"
echo "View logs with: nvidia_fan_controlV2"
