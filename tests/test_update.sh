#!/usr/bin/env bash
# Installer + `nvfc update` against a throw-away DESTDIR (no systemd, no real /etc):
#   * incremental installs touch only what changed and say whether a restart is needed
#   * an old config is upgraded in place and survives updates
#   * `nvfc update` finds the newest tag, downloads, installs, and is a no-op when current
#   * hostile archives are rejected
set -euo pipefail

ROOT_DIR=${ROOT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
TMPDIR=$(mktemp -d)
HTTP_PID=""
trap '[ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true; rm -rf "$TMPDIR"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

DEST="$TMPDIR/root"
export DESTDIR="$DEST"
LIBEXEC="$DEST/usr/local/lib/nvidia-fan-control"
CONF="$DEST/etc/nvidia-fan-control.conf"
export NVFC_LIBEXEC_DIR="$LIBEXEC" NVFC_CONFIG_PATH="$CONF"

# make_tree <dir> <version>: a copy of the sources at a given version
make_tree() {
  mkdir -p "$1"
  (cd "$ROOT_DIR" && tar --exclude=./build --exclude=./.git --exclude='*.so' -cf - .) | tar -xf - -C "$1"
  echo "$2" >"$1/VERSION"
}

# --- 1. fresh install, then a pure re-run ----------------------------------------
make_tree "$TMPDIR/v1" 9.0.0
out=$(sh "$TMPDIR/v1/installer.sh" --no-start 2>&1)
grep -q 'installed 9.0.0' <<<"$out" || fail "fresh install message: $out"
for f in usr/local/sbin/nvidia-fan-controld usr/local/bin/nvfc usr/local/lib/nvidia-fan-control/nvfc_tui.py \
         usr/local/lib/nvidia-fan-control/nvfc_config.py etc/systemd/system/nvidia-fan-control.service; do
  [ -f "$DEST/$f" ] || fail "missing $f"
done
grep -q '^active = balanced' "$CONF" || fail "fresh config should start on balanced"

out=$(sh "$TMPDIR/v1/installer.sh" --no-start 2>&1)
grep -q 'already up to date' <<<"$out" || fail "re-run should change nothing: $out"
grep -q 'does not need a restart' <<<"$out" || fail "re-run should not need a restart"

# --- 2. configurator-only release: no daemon restart ----------------------------
make_tree "$TMPDIR/v2" 9.0.1
echo "# tweak" >>"$TMPDIR/v2/tui/nvfc_tui.py"
cp "$CONF" "$TMPDIR/conf.before"
daemon_before=$(sha256sum "$DEST/usr/local/sbin/nvidia-fan-controld" | cut -c1-16)
out=$(sh "$TMPDIR/v2/installer.sh" --no-start 2>&1)
grep -q 'updated 9.0.0 -> 9.0.1' <<<"$out" || fail "update message: $out"
grep -q 'configurator' <<<"$out" || fail "configurator should be listed as updated"
grep -q 'does not need a restart' <<<"$out" || fail "configurator-only update must not restart the daemon: $out"
grep -q '# tweak' "$LIBEXEC/nvfc_tui.py" || fail "new configurator not installed"
cmp -s "$CONF" "$TMPDIR/conf.before" || fail "update changed the config"
[ "$(cat "$LIBEXEC/VERSION")" = "9.0.1" ] || fail "VERSION not updated"
[ "$(python3 "$DEST/usr/local/bin/nvfc" --version)" = "nvfc 9.0.1" ] || fail "nvfc --version"

# --- 3. daemon change: restart is flagged ---------------------------------------
make_tree "$TMPDIR/v3" 9.0.2
echo "/* changed */" >>"$TMPDIR/v3/src/log.c"
out=$(sh "$TMPDIR/v3/installer.sh" --no-start 2>&1)
grep -q 'needs a restart' <<<"$out" || fail "daemon change should flag a restart: $out"
[ "$(sha256sum "$DEST/usr/local/sbin/nvidia-fan-controld" | cut -c1-16)" != "$daemon_before" ] || fail "daemon not replaced"

# --- 4. an old (schema 1) config is carried forward --------------------------------
cat >"$CONF" <<'CFG'
[curve]
temps = 0,50,70,90
speeds = 20,40,60,100
[run]
sleep_low = 3
[smooth]
ramp_down = slow
CFG
cp "$CONF" "$TMPDIR/old.conf"
make_tree "$TMPDIR/v4" 9.0.3
out=$(sh "$TMPDIR/v4/installer.sh" --no-start 2>&1)
grep -q 'upgraded schema 1 -> 2' <<<"$out" || fail "installer should upgrade the old config: $out"
grep -q 'needs a restart' <<<"$out" || fail "a config upgrade must restart the daemon"
grep -q '^active = custom1' "$CONF" || fail "old settings should be custom1 and active"
grep -q '^temps = 0,50,70,90' "$CONF" || fail "old curve lost"
cmp -s "$TMPDIR/old.conf" "$CONF.v1.bak" || fail "backup of the old config missing"

# --- 5. `nvfc update` -----------------------------------------------------------------
NVFC="python3 $DEST/usr/local/bin/nvfc"
make_tree "$TMPDIR/rel" 9.0.10
mkdir -p "$TMPDIR/www/refs/tags" "$TMPDIR/arch/repo-9.0.10"
cp -a "$TMPDIR/rel/." "$TMPDIR/arch/repo-9.0.10/"
tar -C "$TMPDIR/arch" -czf "$TMPDIR/www/refs/tags/v9.0.10.tar.gz" repo-9.0.10
echo '[{"name":"v9.0.2"},{"name":"v9.0.10"},{"name":"v9.0.9"},{"name":"nightly-build"}]' >"$TMPDIR/www/tags.json"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
(cd "$TMPDIR/www" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
HTTP_PID=$!
for _ in $(seq 50); do python3 -c "import urllib.request as u; u.urlopen('http://127.0.0.1:$PORT/tags.json')" 2>/dev/null && break; sleep 0.1; done
export NVFC_UPDATE_TAGS_URL="http://127.0.0.1:$PORT/tags.json"
export NVFC_UPDATE_ARCHIVE_URL="http://127.0.0.1:$PORT/{ref}.tar.gz"

out=$($NVFC update --check)
grep -q 'Available: 9.0.10' <<<"$out" || fail "should pick v9.0.10 (numeric, not text, order): $out"
grep -q 'Update available' <<<"$out" || fail "--check should report an update"
[ "$(cat "$LIBEXEC/VERSION")" = "9.0.3" ] || fail "--check must not install"

if [ "$(id -u)" -eq 0 ]; then
  out=$($NVFC update --yes 2>&1)
  grep -q 'updated 9.0.3 -> 9.0.10' <<<"$out" || fail "update did not install: $out"
  [ "$(cat "$LIBEXEC/VERSION")" = "9.0.10" ] || fail "installed VERSION"
  grep -q '^temps = 0,50,70,90' "$CONF" || fail "update lost the config"
  out=$($NVFC update --yes 2>&1)
  grep -q 'Already up to date' <<<"$out" || fail "second update should be a no-op: $out"
  # from a folder (e.g. a git checkout)
  make_tree "$TMPDIR/dev" 9.1.0
  out=$($NVFC update --yes --from "$TMPDIR/dev" 2>&1)
  grep -q 'updated 9.0.10 -> 9.1.0' <<<"$out" || fail "--from folder: $out"
fi

# --- 6. hostile archives are refused ---------------------------------------------------
python3 - "$TMPDIR/evil.tar.gz" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w:gz") as t:
    data = b"pwned"
    info = tarfile.TarInfo("../escaped.txt"); info.size = len(data)
    t.addfile(info, io.BytesIO(data))
PY
if $NVFC update --check --from "$TMPDIR/evil.tar.gz" >"$TMPDIR/evil.out" 2>&1; then fail "hostile archive accepted"; fi
grep -q 'Unsafe path' "$TMPDIR/evil.out" || fail "expected unsafe path error: $(cat "$TMPDIR/evil.out")"
[ ! -e "$TMPDIR/escaped.txt" ] || fail "archive escaped its folder"
echo ok
