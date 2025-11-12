#!/bin/sh
# Uninstaller for NVIDIA Fan Control V2.
# Removes the installed daemon, CLI, configuration, and systemd service.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR="$SCRIPT_DIR"
CONFIG_PATH="/etc/nvidia-fan-controlV2.conf"
SERVICE_PATH="/etc/systemd/system/nvidia-fan-controlV2.service"
DAEMON_PATH="/usr/local/sbin/nvidia_fan_controlV2d"
CLI_PATH="/usr/local/bin/nvidia_fan_controlV2"
LIBEXEC_DIR="/usr/local/lib/nvidia-fan-control"

if [ "${NVFC_INSTALLER_ROOT:-0}" -ne 1 ] && [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        echo "[uninstall] Elevating privileges with sudo..."
        exec sudo NVFC_INSTALLER_ROOT=1 "$0" "$@"
    else
        echo "[uninstall] Please run this script as root." >&2
        exit 1
    fi
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop nvidia-fan-controlV2 2>/dev/null || true
    systemctl disable nvidia-fan-controlV2 2>/dev/null || true
fi

rm -f "$DAEMON_PATH"
rm -f "$CLI_PATH"
rm -f "$SERVICE_PATH"
rm -f "$CONFIG_PATH"
rm -rf "$LIBEXEC_DIR"

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi

echo "Removed:"
for item in "$DAEMON_PATH" "$CLI_PATH" "$SERVICE_PATH" "$CONFIG_PATH" "$LIBEXEC_DIR"; do
    echo "  $item"
done

echo "Uninstallation complete."
