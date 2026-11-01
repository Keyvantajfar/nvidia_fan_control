#!/bin/sh
# Uninstaller for NVIDIA Fan Control.
#   ./uninstall.sh          remove everything but keep /etc/nvidia-fan-control.conf
#   ./uninstall.sh --purge  also delete the config
# Pre-1.0 ("V2") files are removed too.

set -eu

CONFIG_PATH=/etc/nvidia-fan-control.conf
LIBEXEC_DIR=/usr/local/lib/nvidia-fan-control
FILES="/usr/local/sbin/nvidia-fan-controld /usr/local/bin/nvfc /usr/local/bin/nvidia-fan-control /etc/systemd/system/nvidia-fan-control.service
/usr/local/sbin/nvidia_fan_controlV2d /usr/local/bin/nvidia_fan_controlV2 /etc/systemd/system/nvidia-fan-controlV2.service"

PURGE=0
for arg in "$@"; do
    case $arg in
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[uninstall] Unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || { echo "[uninstall] Please run as root." >&2; exit 1; }
    exec sudo "$0" "$@"
fi

if [ -d /run/systemd/system ]; then
    for svc in nvidia-fan-control nvidia-fan-controlV2; do
        systemctl disable --now "$svc" >/dev/null 2>&1 || true
    done
fi

# The daemon hands the fan back to the driver (auto mode) when it stops.
# shellcheck disable=SC2086
rm -f $FILES
rm -rf "$LIBEXEC_DIR"
if [ "$PURGE" -eq 1 ]; then
    rm -f "$CONFIG_PATH" /etc/nvidia-fan-controlV2.conf
fi

if [ -d /run/systemd/system ]; then systemctl daemon-reload; fi

echo "NVIDIA Fan Control removed."
if [ "$PURGE" -eq 0 ] && [ -f "$CONFIG_PATH" ]; then
    echo "Config kept at $CONFIG_PATH (use --purge to delete it)."
fi
