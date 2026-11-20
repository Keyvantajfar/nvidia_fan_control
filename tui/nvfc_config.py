#!/usr/bin/env python3
"""Config model shared by the configurator (nvfc_tui.py) and the nvfc CLI.

Stdlib only, no curses.  Owns three things:

* the profile table (quiet / balanced / performance are built in, custom1 and
  custom2 are user slots with a free-form name),
* reading and writing /etc/nvidia-fan-control.conf,
* config migration.  Every config file carries a schema number
  (``[nvfc] config_version``; files without it are schema 1, i.e. everything
  written before profiles existed).  ``MIGRATIONS[n]`` upgrades schema n to n+1,
  and files are walked up one step at a time, so a config from any older release
  is carried forward.  To change the format later: bump CONFIG_VERSION, add one
  ``_migrate_N_to_N+1`` function to MIGRATIONS, and teach the daemon (src/config.c).

    python3 nvfc_config.py migrate [--config PATH]   upgrade the file in place (keeps a .vN.bak)
    python3 nvfc_config.py show    [--config PATH]   print profiles and the active one
"""
from __future__ import annotations

import argparse
import configparser
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

CONFIG_PATH = os.environ.get("NVFC_CONFIG_PATH") or "/etc/nvidia-fan-control.conf"
CONFIG_VERSION = 2

MIN_POINTS = 3
MAX_POINTS = 7
TEMP_MIN = 0
TEMP_MAX = 120
SPEED_MIN = 0
SPEED_MAX = 100
NAME_MAX = 24

BUILTIN_IDS = ["quiet", "balanced", "performance"]
CUSTOM_IDS = ["custom1", "custom2"]
ALL_IDS = BUILTIN_IDS + CUSTOM_IDS
ALIASES = {"cool": "performance", "default": "balanced"}

RAMP_LEVELS = ["quick", "balanced", "slow"]
# Advanced Smart Smooth keys: not shown in the UI, kept in the file when present.
SMOOTH_EXTRA_KEYS = ("deadband", "down_hold", "down_step", "down_interval", "emergency_temp")

DEFAULT_RUN_SETTINGS = {
    "gpu_index": 0,
    "sleep_low": 5,
    "sleep_high": 2,
    "sleep_max": 10,
    "high_temp_threshold": 42,
}

LEGACY_NAME = "Previous config"


@dataclass
class Profile:
    id: str
    name: str
    temps: List[int]
    speeds: List[int]
    smooth_enabled: bool = True
    ramp_down: str = "balanced"
    smooth_extra: Dict[str, str] = field(default_factory=dict)

    @property
    def builtin(self) -> bool:
        return self.id in BUILTIN_IDS

    def copy(self) -> "Profile":
        return Profile(self.id, self.name, list(self.temps), list(self.speeds),
                       self.smooth_enabled, self.ramp_down, dict(self.smooth_extra))


# Keep in sync with k_builtin in src/config.c (tests/test_profile_sync.sh checks it).
_BUILTIN_DATA = [
    ("quiet", "Quiet", [0, 45, 60, 72, 82, 92, 100], [15, 20, 30, 45, 60, 75, 90], "quick"),
    ("balanced", "Balanced", [0, 40, 55, 67, 75, 85], [25, 30, 45, 65, 78, 99], "balanced"),
    ("performance", "Performance", [0, 35, 45, 55, 65, 75, 85], [30, 40, 55, 70, 85, 95, 100], "slow"),
]
BUILTINS: Dict[str, Profile] = {
    pid: Profile(pid, name, temps, speeds, True, ramp) for pid, name, temps, speeds, ramp in _BUILTIN_DATA
}


def default_custom(pid: str) -> Profile:
    """A fresh custom slot: a copy of Balanced."""
    p = BUILTINS["balanced"].copy()
    p.id = pid
    p.name = f"Custom {pid[-1]}"
    return p


@dataclass
class Settings:
    active: str = "balanced"
    run: Dict[str, int] = field(default_factory=lambda: dict(DEFAULT_RUN_SETTINGS))
    custom: Dict[str, Profile] = field(default_factory=lambda: {c: default_custom(c) for c in CUSTOM_IDS})

    def profile(self, pid: str) -> Profile:
        return BUILTINS[pid] if pid in BUILTINS else self.custom[pid]

    def all_profiles(self) -> List[Profile]:
        return [self.profile(pid) for pid in ALL_IDS]


# --- helpers ------------------------------------------------------------------

def clean_name(text: str, fallback: str) -> str:
    text = re.sub(r"[\x00-\x1f\x7f]", " ", text or "")
    text = " ".join(text.split())[:NAME_MAX].strip()
    return text or fallback


def validate_curve(temps: List[int], speeds: List[int]) -> Tuple[bool, str]:
    if not (MIN_POINTS <= len(temps) <= MAX_POINTS):
        return False, f"Curve must contain between {MIN_POINTS} and {MAX_POINTS} points."
    if len(temps) != len(speeds):
        return False, "Temperature and speed point counts must match."
    last_temp = -1
    for idx, temp in enumerate(temps):
        if not (TEMP_MIN <= temp <= TEMP_MAX):
            return False, f"Temperature at index {idx} must be within {TEMP_MIN}-{TEMP_MAX}."
        if temp <= last_temp:
            return False, "Temperatures must be strictly increasing."
        last_temp = temp
    for idx, speed in enumerate(speeds):
        if not (SPEED_MIN <= speed <= SPEED_MAX):
            return False, f"Speed at index {idx} must be within {SPEED_MIN}-{SPEED_MAX}."
    return True, ""


def resolve_profile(settings: Settings, text: str) -> Optional[str]:
    """Map what the user typed (id, alias, or a custom profile's name) to an id."""
    key = (text or "").strip().lower()
    key = ALIASES.get(key, key)
    if key in ALL_IDS:
        return key
    for pid in CUSTOM_IDS:
        if settings.custom[pid].name.lower() == key:
            return pid
    return None


def _ints(raw: str) -> List[int]:
    return [int(x.strip()) for x in raw.split(",") if x.strip()]


def _new_parser() -> configparser.ConfigParser:
    # interpolation off: a custom profile name may contain '%'. strict off: a
    # hand-edited file with a repeated key should still load.
    return configparser.ConfigParser(interpolation=None, strict=False)


# --- migrations -----------------------------------------------------------------

def _migrate_1_to_2(p: configparser.ConfigParser) -> None:
    """Schema 1 (single [curve] + [smooth]) -> profiles.

    The old settings become profile "custom1" and stay active, so the fan behaves
    exactly as before the upgrade. [run] is untouched (it is global)."""
    curve = dict(p["curve"]) if p.has_section("curve") else {}
    smooth = dict(p["smooth"]) if p.has_section("smooth") else {}
    for section in ("curve", "smooth"):
        p.remove_section(section)

    run = dict(p["run"]) if p.has_section("run") else None
    if run is not None:
        p.remove_section("run")

    p["nvfc"] = {"config_version": "2"}
    if curve or smooth:
        p["profile"] = {"active": "custom1"}
    else:
        p["profile"] = {"active": "balanced"}  # nothing was customised
    if run is not None:
        p["run"] = run
    if curve or smooth:
        c1 = {"name": LEGACY_NAME}
        for key in ("temps", "speeds"):
            if key in curve:
                c1[key] = curve[key]
        for key in ("enabled", "ramp_down") + SMOOTH_EXTRA_KEYS:
            if key in smooth:
                c1[key] = smooth[key]
        p["custom1"] = c1


MIGRATIONS: Dict[int, Callable[[configparser.ConfigParser], None]] = {1: _migrate_1_to_2}


def _schema_of(p: configparser.ConfigParser) -> int:
    try:
        return p.getint("nvfc", "config_version", fallback=1)
    except ValueError:
        return 1


def _upgrade(p: configparser.ConfigParser) -> int:
    """Walk the parser up to CONFIG_VERSION in place. Returns the schema it started at."""
    start = version = _schema_of(p)
    while version < CONFIG_VERSION:
        MIGRATIONS[version](p)
        version += 1
    return start


# --- load / save ----------------------------------------------------------------

def _profile_from_section(pid: str, sec: configparser.SectionProxy) -> Profile:
    p = default_custom(pid)
    p.name = clean_name(sec.get("name", ""), p.name)
    try:
        temps, speeds = _ints(sec.get("temps", "")), _ints(sec.get("speeds", ""))
        if validate_curve(temps, speeds)[0]:
            p.temps, p.speeds = temps, speeds
    except ValueError:
        pass
    try:
        p.smooth_enabled = sec.getboolean("enabled", fallback=True)
    except ValueError:
        pass
    ramp = sec.get("ramp_down", "balanced").strip().lower()
    p.ramp_down = ramp if ramp in RAMP_LEVELS else "balanced"
    for key in SMOOTH_EXTRA_KEYS:
        if key in sec:
            p.smooth_extra[key] = sec[key]
    return p


def load_settings(path: str) -> Tuple[Settings, int]:
    """Read `path` (any schema). Returns (settings, schema the file was written with).
    A missing file gives the defaults (schema == CONFIG_VERSION)."""
    settings = Settings()
    if not os.path.exists(path):
        return settings, CONFIG_VERSION
    p = _new_parser()
    try:
        p.read(path, encoding="utf-8")
    except configparser.Error:
        return settings, CONFIG_VERSION  # unreadable: start from defaults, never crash the UI
    start = _upgrade(p)

    active = p.get("profile", "active", fallback="balanced").strip().lower()
    settings.active = active if active in ALL_IDS else "balanced"
    for key, default in DEFAULT_RUN_SETTINGS.items():
        try:
            settings.run[key] = p.getint("run", key, fallback=default)
        except ValueError:
            settings.run[key] = default
    for pid in CUSTOM_IDS:
        if p.has_section(pid):
            settings.custom[pid] = _profile_from_section(pid, p[pid])
    return settings, start


def _write_atomic(path: str, text: str) -> None:
    dirpath = os.path.dirname(path) or "."
    os.makedirs(dirpath, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dirpath, prefix=".nvfc-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fp:
            fp.write(text)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _backup_old_schema(path: str, schema: int) -> Optional[str]:
    """Copy a file written by an older schema aside, once. Returns the backup path."""
    if schema >= CONFIG_VERSION or not os.path.exists(path):
        return None
    bak = f"{path}.v{schema}.bak"
    if os.path.exists(bak):
        return bak
    with open(path, "rb") as src:
        data = src.read()
    with open(bak, "wb") as dst:
        dst.write(data)
    return bak


def render_settings(settings: Settings) -> str:
    p = _new_parser()
    p["nvfc"] = {"config_version": str(CONFIG_VERSION)}
    p["profile"] = {"active": settings.active}
    p["run"] = {k: str(settings.run[k]) for k in DEFAULT_RUN_SETTINGS}
    for pid in CUSTOM_IDS:
        c = settings.custom[pid]
        p[pid] = {
            "name": clean_name(c.name, f"Custom {pid[-1]}"),
            "temps": ",".join(str(x) for x in c.temps),
            "speeds": ",".join(str(x) for x in c.speeds),
            "enabled": "on" if c.smooth_enabled else "off",
            "ramp_down": c.ramp_down,
            **c.smooth_extra,
        }
    import io
    buf = io.StringIO()
    p.write(buf)
    return buf.getvalue()


def save_settings(path: str, settings: Settings, schema_before: int = CONFIG_VERSION) -> Optional[str]:
    """Write the config in the current schema. If the file on disk was written by an
    older schema it is backed up first; returns the backup path in that case."""
    bak = _backup_old_schema(path, schema_before)
    _write_atomic(path, render_settings(settings))
    return bak


def migrate_file(path: str) -> Optional[str]:
    """Upgrade `path` to the current schema in place. Returns a message describing
    what was done, or None when nothing needed doing (missing file, already current)."""
    if not os.path.exists(path):
        return None
    p = _new_parser()
    try:
        p.read(path, encoding="utf-8")
    except configparser.Error as exc:
        return f"{path}: could not parse ({exc.__class__.__name__}); left untouched"
    schema = _schema_of(p)
    if schema > CONFIG_VERSION:
        return f"{path}: written by a newer version (schema {schema}); left untouched"
    if schema == CONFIG_VERSION:
        return None
    settings, _ = load_settings(path)
    bak = save_settings(path, settings, schema)
    where = "custom1 (\"%s\")" % LEGACY_NAME if settings.active == "custom1" else settings.active
    return f"{path}: upgraded schema {schema} -> {CONFIG_VERSION}; your settings are in profile {where}; backup: {bak}"


def set_active(path: str, pid: str) -> None:
    settings, schema = load_settings(path)
    settings.active = pid
    save_settings(path, settings, schema)


# --- command line ---------------------------------------------------------------

def _describe(settings: Settings) -> str:
    lines = []
    for prof in settings.all_profiles():
        mark = "*" if prof.id == settings.active else " "
        kind = "built-in" if prof.builtin else "custom"
        lines.append(f" {mark} {prof.id:<12} {prof.name:<{NAME_MAX}} {kind}")
    return "\n".join(lines)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="NVIDIA Fan Control config tools")
    ap.add_argument("command", choices=["migrate", "show"])
    ap.add_argument("--config", default=CONFIG_PATH)
    args = ap.parse_args(argv)
    try:
        if args.command == "migrate":
            msg = migrate_file(args.config)
            print(msg or f"{args.config}: config already up to date")
        else:
            settings, _ = load_settings(args.config)
            print(_describe(settings))
    except OSError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
