#!/bin/sh
# One-step installer / updater for NVIDIA Fan Control.
#
#   ./installer.sh              install or update: build, install what changed, (re)start only if needed
#   ./installer.sh --no-start   install only
#   ./installer.sh --configure  open the configurator before starting
#
# Needs: python3, plus gcc (or cc) and make when the daemon has to be (re)built.
# The NVIDIA driver provides NVML at runtime; no NVIDIA dev headers are required.
#
# Safe to re-run, and this is also how you update (or run `nvfc update`):
#   * only the parts that changed are replaced (daemon, configurator, CLI, unit);
#   * the service is restarted only when the daemon or its unit changed, so a
#     configurator-only update never touches the running fan control;
#   * your config is never overwritten; an older config is upgraded in place
#     (the old file is kept as <config>.v<N>.bak), see tui/nvfc_config.py;
#   * old "V2" installs are migrated automatically.
#
# DESTDIR=/some/dir installs into that tree without touching systemd (packaging, tests).

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
VERSION=$(cat "$SCRIPT_DIR/VERSION")
DESTDIR=${DESTDIR:-}

SERVICE=nvidia-fan-control
CONFIG_PATH=$DESTDIR/etc/nvidia-fan-control.conf
DAEMON_PATH=$DESTDIR/usr/local/sbin/nvidia-fan-controld
CLI_PATH=$DESTDIR/usr/local/bin/nvfc
CLI_ALIAS=$DESTDIR/usr/local/bin/nvidia-fan-control
UNIT_PATH=$DESTDIR/etc/systemd/system/$SERVICE.service
LIBEXEC_DIR=$DESTDIR/usr/local/lib/nvidia-fan-control

# Pre-1.0 ("V2") names, migrated and removed on install.
OLD_SERVICE=nvidia-fan-controlV2
OLD_CONFIG=$DESTDIR/etc/nvidia-fan-controlV2.conf
OLD_FILES="$DESTDIR/usr/local/sbin/nvidia_fan_controlV2d $DESTDIR/usr/local/bin/nvidia_fan_controlV2 $DESTDIR/etc/systemd/system/$OLD_SERVICE.service"

START=1
CONFIGURE=0
for arg in "$@"; do
    case $arg in
        --no-start) START=0 ;;
        --configure) CONFIGURE=1 ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[installer] Unknown option: $arg" >&2; exit 1 ;;
    esac
done

say() { echo "[installer] $*"; }
die() { echo "[installer] $*" >&2; exit 1; }

# --- privileges -------------------------------------------------------------
if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || die "Please run as root (sudo not found)."
    say "Elevating with sudo..."
    exec sudo "$0" "$@"
fi
SYSTEMD=0
if [ -z "$DESTDIR" ] && [ -d /run/systemd/system ]; then SYSTEMD=1; fi

# --- what is installed now, what do we have ---------------------------------
sumof() { if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1; else cksum | tr ' ' -; fi; }
# Fingerprint of everything that ends up inside the daemon except VERSION: a
# release that only changes the configurator keeps the same daemon code.
NEW_SUM=$(cd "$SCRIPT_DIR" && cat src/*.c src/*.h Makefile | sumof)
OLD_SUM=$(cat "$LIBEXEC_DIR/code.sum" 2>/dev/null || echo none)
OLD_VERSION=$(cat "$LIBEXEC_DIR/VERSION" 2>/dev/null || echo none)

NEED_BUILD=0
RESTART=0
if [ ! -x "$DAEMON_PATH" ] || [ "$NEW_SUM" != "$OLD_SUM" ]; then
    NEED_BUILD=1; RESTART=1       # new or changed daemon code
elif [ "$VERSION" != "$OLD_VERSION" ]; then
    NEED_BUILD=1                  # same code: rebuild only for the version string, no restart
fi

# --- dependencies -----------------------------------------------------------
missing=""
command -v python3 >/dev/null 2>&1 || missing="$missing python3"
if command -v python3 >/dev/null 2>&1 && ! python3 -c 'import curses' >/dev/null 2>&1; then
    missing="$missing python3-curses"
fi
CC_BIN=""
if [ "$NEED_BUILD" -eq 1 ]; then
    for c in gcc cc; do command -v "$c" >/dev/null 2>&1 && CC_BIN=$c && break; done
    [ -n "$CC_BIN" ] || missing="$missing gcc"
    command -v make >/dev/null 2>&1 || missing="$missing make"
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
if [ "$NEED_BUILD" -eq 1 ]; then
    say "Building $VERSION..."
    make -s -C "$SCRIPT_DIR" BUILD_DIR="$BUILD_TMP" CC="$CC_BIN" build
    [ -f "$BUILD_TMP/nvidia-fan-controld" ] || die "Build failed."
fi

# --- migrate a pre-1.0 install ---------------------------------------------
if [ "$SYSTEMD" -eq 1 ]; then
    systemctl disable --now "$OLD_SERVICE" >/dev/null 2>&1 || true
fi
if [ ! -f "$CONFIG_PATH" ] && [ -f "$OLD_CONFIG" ]; then
    say "Migrating $OLD_CONFIG -> $CONFIG_PATH"
    install -d "$(dirname "$CONFIG_PATH")"
    cp "$OLD_CONFIG" "$CONFIG_PATH"
fi
# shellcheck disable=SC2086
rm -f $OLD_FILES "$OLD_CONFIG"

# --- install only what changed ----------------------------------------------
CHANGED=""
# put MODE SRC DST NAME: replace DST (atomically, so a running daemon is not
# disturbed) when its content differs.
put() {
    if [ -f "$3" ] && cmp -s "$2" "$3"; then return 0; fi
    install -d "$(dirname "$3")"
    install -m "$1" "$2" "$3.new.$$"
    mv -f "$3.new.$$" "$3"
    CHANGED="$CHANGED $4"
}

install -d "$LIBEXEC_DIR"
if [ "$NEED_BUILD" -eq 1 ]; then
    put 755 "$BUILD_TMP/nvidia-fan-controld" "$DAEMON_PATH" daemon
fi
put 755 "$SCRIPT_DIR/bin/nvfc" "$CLI_PATH" cli
ln -sf nvfc "$CLI_ALIAS"
put 755 "$SCRIPT_DIR/tui/nvfc_tui.py" "$LIBEXEC_DIR/nvfc_tui.py" configurator
put 644 "$SCRIPT_DIR/tui/nvfc_config.py" "$LIBEXEC_DIR/nvfc_config.py" config-module
put 644 "$SCRIPT_DIR/systemd/$SERVICE.service" "$UNIT_PATH" unit
case "$CHANGED" in *unit*) RESTART=1 ;; esac
put 644 "$SCRIPT_DIR/VERSION" "$LIBEXEC_DIR/VERSION" version
printf '%s\n' "$NEW_SUM" >"$LIBEXEC_DIR/code.sum"

# --- config: create, or carry an older one forward ---------------------------
if [ ! -f "$CONFIG_PATH" ]; then
    say "Writing default config to $CONFIG_PATH"
    install -d "$(dirname "$CONFIG_PATH")"
    cat >"$CONFIG_PATH" <<'CONF'
[nvfc]
config_version = 2

[profile]
active = balanced

[run]
gpu_index = 0
sleep_low = 5
sleep_high = 2
sleep_max = 10
high_temp_threshold = 42
CONF
    chmod 644 "$CONFIG_PATH"
else
    MIGRATION=$(python3 -I "$LIBEXEC_DIR/nvfc_config.py" migrate --config "$CONFIG_PATH" 2>&1) || \
        say "Warning: could not check the config format: $MIGRATION"
    case "$MIGRATION" in
        *upgraded*)
            say "$MIGRATION"
            CHANGED="$CHANGED config"
            RESTART=1 ;;  # a reload is not enough: a running daemon may predate the new format
    esac
fi

if [ "$CONFIGURE" -eq 1 ] && [ -z "$DESTDIR" ]; then
    python3 "$LIBEXEC_DIR/nvfc_tui.py" --config "$CONFIG_PATH" || say "Configurator closed without saving."
fi

# --- start / restart only when needed -----------------------------------------
if [ "$RESTART" -eq 1 ]; then
    say "Daemon code, unit or config format changed: the service needs a restart."
else
    say "Daemon unchanged: the running service does not need a restart."
fi
if [ "$SYSTEMD" -eq 1 ]; then
    systemctl daemon-reload
    if [ "$START" -eq 1 ]; then
        systemctl enable "$SERVICE" >/dev/null 2>&1
        if ! systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
            systemctl restart "$SERVICE"; NOTE="started"
        elif [ "$RESTART" -eq 1 ]; then
            systemctl restart "$SERVICE"; NOTE="restarted"
        else
            NOTE="left running, daemon unchanged"
        fi
        sleep 1
        say "Service: $(systemctl is-active "$SERVICE" 2>/dev/null || true) ($NOTE)"
    else
        say "Not started. Run: sudo systemctl enable --now $SERVICE"
    fi
elif [ -z "$DESTDIR" ]; then
    say "systemd not found; run the daemon yourself: $DAEMON_PATH --config $CONFIG_PATH"
fi

if [ -n "$CHANGED" ]; then
    say "Updated:$CHANGED"
else
    say "Everything was already up to date."
fi

if [ "$OLD_VERSION" = "none" ]; then
    HEADLINE="installed $VERSION"
elif [ "$OLD_VERSION" = "$VERSION" ]; then
    HEADLINE="$VERSION is up to date"
else
    HEADLINE="updated $OLD_VERSION -> $VERSION"
fi

cat <<MSG

NVIDIA Fan Control $HEADLINE.

  nvfc                 open the configurator (profiles, curve, Smart Smooth, polling)
  nvfc profile NAME    switch profile (quiet, balanced, performance, custom1, custom2)
  nvfc status          live temperature / fan speed
  nvfc update          update to the latest release (keeps your config)
  nvfc --help          everything else
MSG
