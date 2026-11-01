#!/bin/sh
# One-step installer for NVIDIA Fan Control.
#
#   ./installer.sh              build, install, enable and start the service
#   ./installer.sh --no-start   install only
#   ./installer.sh --configure  open the configurator before starting
#
# Needs: gcc (or cc), make, python3. The NVIDIA driver provides NVML at runtime;
# no NVIDIA dev headers are required. Safe to re-run (it upgrades in place and
# keeps your config). Old "V2" installs are migrated automatically.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VERSION=$(cat "$SCRIPT_DIR/VERSION")

SERVICE=nvidia-fan-control
CONFIG_PATH=/etc/nvidia-fan-control.conf
DAEMON_PATH=/usr/local/sbin/nvidia-fan-controld
CLI_PATH=/usr/local/bin/nvfc
CLI_ALIAS=/usr/local/bin/nvidia-fan-control
UNIT_PATH=/etc/systemd/system/$SERVICE.service
LIBEXEC_DIR=/usr/local/lib/nvidia-fan-control

# Pre-1.0 ("V2") names, migrated and removed on install.
OLD_SERVICE=nvidia-fan-controlV2
OLD_CONFIG=/etc/nvidia-fan-controlV2.conf
OLD_FILES="/usr/local/sbin/nvidia_fan_controlV2d /usr/local/bin/nvidia_fan_controlV2 /etc/systemd/system/$OLD_SERVICE.service"

START=1
CONFIGURE=0
for arg in "$@"; do
    case $arg in
        --no-start) START=0 ;;
        --configure) CONFIGURE=1 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[installer] Unknown option: $arg" >&2; exit 1 ;;
    esac
done

say() { echo "[installer] $*"; }
die() { echo "[installer] $*" >&2; exit 1; }

# --- privileges -------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || die "Please run as root (sudo not found)."
    say "Elevating with sudo..."
    exec sudo "$0" "$@"
fi

# --- dependencies -----------------------------------------------------------
missing=""
CC_BIN=""
for c in gcc cc; do command -v "$c" >/dev/null 2>&1 && CC_BIN=$c && break; done
[ -n "$CC_BIN" ] || missing="$missing gcc"
command -v make >/dev/null 2>&1 || missing="$missing make"
command -v python3 >/dev/null 2>&1 || missing="$missing python3"
if command -v python3 >/dev/null 2>&1 && ! python3 -c 'import curses' >/dev/null 2>&1; then
    missing="$missing python3-curses"
fi
if [ -n "$missing" ]; then
    echo "[installer] Missing:$missing" >&2
    echo "  Debian/Ubuntu: sudo apt install build-essential python3" >&2
    echo "  Fedora:        sudo dnf install gcc make python3" >&2
    echo "  Arch:          sudo pacman -S base-devel python" >&2
    exit 1
fi
if command -v ldconfig >/dev/null 2>&1 && ! ldconfig -p 2>/dev/null | grep -q 'libnvidia-ml\.so\.1'; then
    say "Warning: libnvidia-ml.so.1 not found - is the NVIDIA driver installed? Continuing anyway."
fi

# --- build (in a scratch dir, so the source folder stays clean) -------------
BUILD_TMP=$(mktemp -d)
trap 'rm -rf "$BUILD_TMP"' EXIT
say "Building $VERSION..."
make -s -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_TMP" CC="$CC_BIN" build
[ -f "$BUILD_TMP/nvidia-fan-controld" ] || die "Build failed."

# --- migrate a pre-1.0 install ---------------------------------------------
if [ -d /run/systemd/system ]; then
    systemctl disable --now "$OLD_SERVICE" >/dev/null 2>&1 || true
fi
if [ ! -f "$CONFIG_PATH" ] && [ -f "$OLD_CONFIG" ]; then
    say "Migrating $OLD_CONFIG -> $CONFIG_PATH"
    cp "$OLD_CONFIG" "$CONFIG_PATH"
fi
# shellcheck disable=SC2086
rm -f $OLD_FILES "$OLD_CONFIG"

# --- install ----------------------------------------------------------------
install -d "$LIBEXEC_DIR"
install -m 755 "$BUILD_TMP/nvidia-fan-controld" "$DAEMON_PATH"
install -m 755 "$SCRIPT_DIR/bin/nvfc" "$CLI_PATH"
ln -sf nvfc "$CLI_ALIAS"
install -m 755 "$SCRIPT_DIR/tui/nvfc_tui.py" "$LIBEXEC_DIR/nvfc_tui.py"
install -m 644 "$SCRIPT_DIR/VERSION" "$LIBEXEC_DIR/VERSION"
install -m 644 "$SCRIPT_DIR/systemd/$SERVICE.service" "$UNIT_PATH"

if [ ! -f "$CONFIG_PATH" ]; then
    say "Writing default config to $CONFIG_PATH"
    cat >"$CONFIG_PATH" <<'CONF'
[curve]
temps = 0,40,55,67,75,85
speeds = 25,30,45,65,78,99

[run]
gpu_index = 0
sleep_low = 5
sleep_high = 2
sleep_max = 10
high_temp_threshold = 42

[smooth]
enabled = on
CONF
    chmod 644 "$CONFIG_PATH"
fi

if [ "$CONFIGURE" -eq 1 ]; then
    python3 "$LIBEXEC_DIR/nvfc_tui.py" --config "$CONFIG_PATH" || say "Configurator closed without saving."
fi

# --- start ------------------------------------------------------------------
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    if [ "$START" -eq 1 ]; then
        systemctl enable "$SERVICE" >/dev/null 2>&1
        systemctl restart "$SERVICE"
        sleep 1
        say "Service: $(systemctl is-active "$SERVICE" 2>/dev/null || true)"
    else
        say "Not started. Run: sudo systemctl enable --now $SERVICE"
    fi
else
    say "systemd not found; run the daemon yourself: $DAEMON_PATH --config $CONFIG_PATH"
fi

cat <<MSG

NVIDIA Fan Control $VERSION installed.

  nvfc            open the configurator (edit curve, Smart Smooth, polling)
  nvfc status     live temperature / fan speed
  nvfc logs       follow the log
  nvfc --help     everything else
MSG
