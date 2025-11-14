#!/bin/sh
# Installer for the NVIDIA Fan Control V2 tooling.
#
# This script launches the TUI configurator (if needed), builds the daemon, and
# installs the binary, CLI wrapper, helper scripts, and systemd service.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR="$SCRIPT_DIR"
CONFIG_PATH="/etc/nvidia-fan-controlV2.conf"
BUILD_DIR="$REPO_DIR/build"
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

USE_MOCK="${USE_MOCK_NVML:-0}"
if [ "$USE_MOCK" = "1" ]; then
    echo "[installer] Building with mock NVML support"
    USE_MOCK_NVML=1 make -C "$REPO_DIR" build
else
    make -C "$REPO_DIR" build
fi

if [ ! -f "$BINARY_PATH" ]; then
    echo "[installer] Build failed: $BINARY_PATH not found" >&2
    exit 1
fi

install -m 755 "$BINARY_PATH" /usr/local/sbin/nvidia_fan_controlV2d
install -m 755 "$REPO_DIR/bin/nvidia_fan_controlV2" /usr/local/bin/nvidia_fan_controlV2
install -m 644 "$REPO_DIR/systemd/nvidia-fan-controlV2.service" /etc/systemd/system/nvidia-fan-controlV2.service

install -m 755 "$REPO_DIR/tui/nvfc_tui.py" "$LIBEXEC_DIR/nvfc_tui.py"
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
echo "Enable and start with: systemctl enable --now nvidia-fan-controlV2"
echo "View logs with: nvidia_fan_controlV2"
