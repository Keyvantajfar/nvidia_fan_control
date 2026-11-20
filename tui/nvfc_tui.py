#!/usr/bin/env python3
"""Curses-based configurator for NVIDIA Fan Control.

Interactive terminal UI for editing fan profiles (curve + Smart Smooth) and the
polling options stored in the daemon's INI config file (applied with a reload, no
rebuild).  Dependency-free (stdlib only); works on a console or over SSH.

Key bindings (also shown in the UI):
    ↑/↓   Move between curve points (or move a grabbed point)
    ←/→   Adjust the currently focused value (temperature or speed)
    TAB   Cycle focus between temperature, speed, and runtime parameter panes
    SPACE Pick up / drop the current point for reordering
    A/R   Add or remove a point (between 3 and 7 points)
    P     Pick the profile to view/edit (it becomes the active one when you save)
    C     Copy the viewed profile into a custom slot (custom1 / custom2)
    N     Rename a custom profile
    S     Save
    Q     Quit without saving

Quiet, Balanced and Performance are built in and read-only; custom1 and custom2
are yours to edit and name.  The runtime settings (GPU, polling) are shared by
all profiles.

In the runtime settings pane, ←/→ (or SPACE) toggles Smart Smooth and picks the
ramp-down level (quick / balanced / slow).  While the
daemon runs, the plot shows a live @ marker for the current temperature/fan.

The TUI persists configuration to /etc/nvidia-fan-control.conf as a simple INI
file.
"""
from __future__ import annotations

import argparse
import curses
import os
import shutil
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

from nvfc_config import (
    ALL_IDS, BUILTIN_IDS, CONFIG_VERSION, CUSTOM_IDS, MAX_POINTS, MIN_POINTS, NAME_MAX,
    RAMP_LEVELS, SPEED_MAX, SPEED_MIN, TEMP_MAX, TEMP_MIN, Profile, Settings, clean_name,
    load_settings, save_settings, validate_curve,
)

ACTIVE_SCREEN = None

def safe_addstr(win, y, x, text, attr=None):
    try:
        if attr is None:
            win.addstr(y, x, text)
        else:
            win.addstr(y, x, text, attr)
    except curses.error:
        pass



CONFIG_PATH = "/etc/nvidia-fan-control.conf"
STATUS_PATH = os.environ.get("NVFC_STATUS_PATH") or "/run/nvidia-fan-control/status"
STATUS_MAX_AGE = 90  # daemon rewrites at least every 60 s


def read_version() -> str:
    here = Path(__file__).resolve().parent
    for candidate in (here / "VERSION", here.parent / "VERSION"):
        try:
            return candidate.read_text().strip()
        except OSError:
            continue
    return ""


def read_live_status():
    """Return (temp, fan, target) from the daemon's status file, or None if stale/missing."""
    try:
        data = dict(line.split("=", 1) for line in Path(STATUS_PATH).read_text().splitlines() if "=" in line)
        if time.time() - int(data["ts"]) > STATUS_MAX_AGE:
            return None
        return int(data["temp"]), int(data["fan"]), int(data["target"])
    except (OSError, KeyError, ValueError):
        return None


def interpolate(temps: List[int], speeds: List[int], t: float) -> float:
    """Same curve the daemon uses when Smart Smooth is on."""
    if t <= temps[0]:
        return speeds[0]
    if t >= temps[-1]:
        return speeds[-1]
    for i in range(len(temps) - 1):
        if t < temps[i + 1]:
            frac = (t - temps[i]) / (temps[i + 1] - temps[i])
            return speeds[i] + (speeds[i + 1] - speeds[i]) * frac
    return speeds[-1]


@dataclass
class CurveState:
    """What the screen shows: the viewed profile's curve + smooth settings, plus the
    runtime settings shared by every profile.  `settings` keeps the stored profiles."""
    temps: List[int]
    speeds: List[int]
    settings: Settings = field(default_factory=Settings)
    schema: int = CONFIG_VERSION
    profile_id: str = "balanced"
    gpu_index: int = 0
    sleep_low: int = 5
    sleep_high: int = 2
    sleep_max: int = 10
    high_temp_threshold: int = 42
    smooth_enabled: bool = True
    ramp_down: str = "balanced"
    smooth_extra: dict = field(default_factory=dict)
    selected_index: int = 0
    focus_axis: int = 0  # 0 -> temperature, 1 -> speed
    focus_area: str = "points"  # "points" or "fields"
    field_index: int = 0
    dragging: bool = False
    drag_origin: int = 0
    drag_target: int = 0
    status_message: str = ""
    error_message: str = ""

    @property
    def num_points(self) -> int:
        return len(self.temps)

    @property
    def read_only(self) -> bool:
        return self.profile_id in BUILTIN_IDS

    @property
    def profile(self) -> Profile:
        return self.settings.profile(self.profile_id)

    def commit(self) -> None:
        """Write the on-screen values back into the stored custom profile."""
        if self.read_only:
            return
        p = self.settings.custom[self.profile_id]
        p.temps, p.speeds = list(self.temps), list(self.speeds)
        p.smooth_enabled, p.ramp_down = self.smooth_enabled, self.ramp_down
        p.smooth_extra = dict(self.smooth_extra)

    def view(self, pid: str) -> None:
        """Show profile `pid` (the current one must already be committed)."""
        p = self.settings.profile(pid)
        self.profile_id = pid
        self.temps, self.speeds = list(p.temps), list(p.speeds)
        self.smooth_enabled, self.ramp_down = p.smooth_enabled, p.ramp_down
        self.smooth_extra = dict(p.smooth_extra)
        self.selected_index = min(self.selected_index, self.num_points - 1)
        self.dragging = False


def load_config(path: str) -> CurveState:
    settings, schema = load_settings(path)
    state = CurveState([], [], settings=settings, schema=schema)
    for key, value in settings.run.items():
        setattr(state, key, value)
    state.view(settings.active)
    if schema < CONFIG_VERSION:
        state.status_message = "Config from an older version: your settings are in the custom1 profile."
    return state


def save_config(path: str, state: CurveState) -> None:
    state.commit()
    settings = state.settings
    settings.active = state.profile_id
    for key in settings.run:
        settings.run[key] = getattr(state, key)
    save_settings(path, settings, state.schema)


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, value))


def validate_state(state: CurveState) -> Tuple[bool, str]:
    ok, err = validate_curve(state.temps, state.speeds)
    if not ok:
        return False, err
    if state.sleep_low < 1 or state.sleep_high < 1 or state.sleep_max < 1:
        return False, "Sleep intervals must be positive integers."
    if state.sleep_max < state.sleep_low:
        return False, "Sleep max must be >= sleep low."
    if state.high_temp_threshold < TEMP_MIN or state.high_temp_threshold > TEMP_MAX:
        return False, f"High temperature threshold must fall within {TEMP_MIN}-{TEMP_MAX}°C."
    if state.gpu_index < 0:
        return False, "GPU index cannot be negative."
    return True, ""


def draw_profile_bar(stdscr: "curses._CursesWindow", row: int, width: int, state: CurveState) -> None:
    """One line listing all profiles; the viewed one is highlighted, '*' marks read-only."""
    safe_addstr(stdscr, row, 0, " " * (width - 1))
    col = 0
    safe_addstr(stdscr, row, col, "Profile: ")
    col += 9
    for pid in ALL_IDS:
        prof = state.settings.profile(pid) if pid != state.profile_id else state.profile
        label = f" {prof.name[:14]}{'*' if prof.builtin else ''} "
        attr = curses.A_REVERSE if pid == state.profile_id else curses.A_NORMAL
        safe_addstr(stdscr, row, col, label, attr)
        col += len(label) + 1
    note = "read-only (C = copy to custom)" if state.read_only else "custom"
    safe_addstr(stdscr, row, col + 1, note)


def draw_top_bar(stdscr: "curses._CursesWindow", width: int, state: CurveState) -> None:
    version = read_version()
    title = f" NVIDIA Fan Control {version} Configurator ".replace("  ", " ")
    hints = "↑↓ move  ←→ adjust  SPACE grab  TAB focus  A/R add/remove  P profile  C copy  N rename  S save  Q quit"
    stdscr.attron(curses.A_REVERSE)
    safe_addstr(stdscr, 0, 0, title.ljust(width))
    stdscr.attroff(curses.A_REVERSE)
    safe_addstr(stdscr, 1, 0, hints.ljust(width))
    draw_profile_bar(stdscr, 2, width, state)


def draw_points_table(stdscr: "curses._CursesWindow", start_row: int, width: int, state: CurveState) -> int:
    safe_addstr(stdscr, start_row, 0, "Index | Temp(°C) | Speed(%)")
    safe_addstr(stdscr, start_row + 1, 0, "------+----------+---------")
    for idx, (temp, speed) in enumerate(zip(state.temps, state.speeds)):
        highlight = curses.A_REVERSE if state.focus_area == "points" and idx == state.selected_index else curses.A_NORMAL
        line = f"  {idx:<2} |   {temp:>3}    |   {speed:>3}  "
        safe_addstr(stdscr, start_row + 2 + idx, 0, line.ljust(width), highlight)
        if state.focus_area == "points" and idx == state.selected_index:
            caret_row = start_row + 2 + idx
            caret_col = 9 if state.focus_axis == 0 else 21
            safe_addstr(stdscr, caret_row, caret_col, "↑", curses.A_BOLD)
    return start_row + 2 + state.num_points


def draw_run_settings(stdscr: "curses._CursesWindow", start_row: int, width: int, state: CurveState) -> int:
    safe_addstr(stdscr, start_row, 0, "Runtime settings (TAB to edit)")
    labels = [
        ("GPU index", state.gpu_index),
        ("Sleep low (s)", state.sleep_low),
        ("Sleep high (s)", state.sleep_high),
        ("Sleep max (s)", state.sleep_max),
        ("High temp threshold", state.high_temp_threshold),
        ("Smart Smooth", "ON " if state.smooth_enabled else "OFF"),
        ("Ramp-down", state.ramp_down),
    ]
    for idx, (label, value) in enumerate(labels):
        highlight = curses.A_REVERSE if state.focus_area == "fields" and idx == state.field_index else curses.A_NORMAL
        safe_addstr(stdscr, start_row + 1 + idx, 0, f"  {label:<18}: {value:<4}".ljust(width), highlight)
    return start_row + 1 + len(labels)


def draw_plot(stdscr: "curses._CursesWindow", start_row: int, start_col: int, height: int, width: int, state: CurveState) -> None:
    # Draw axes
    safe_addstr(stdscr, start_row, start_col, "+" + "-" * (width - 1))
    for i in range(1, height):
        safe_addstr(stdscr, start_row + i, start_col, "|")
    safe_addstr(stdscr, start_row + height, start_col, "+" + "-" * (width - 1))
    mode = "smooth (interpolated)" if state.smooth_enabled else "stepped"
    safe_addstr(stdscr, start_row + height + 1, start_col, f"Temp (°C)   curve: {mode}")
    live = read_live_status()
    live_text = f"   live: {live[0]}°C -> fan {live[1]}%" if live and live[1] >= 0 else ""
    safe_addstr(stdscr, start_row - 1, start_col, "Speed (%)" + live_text)

    if not state.temps:
        return

    temp_view_max = 100 if max(state.temps) <= 100 else TEMP_MAX
    temp_range = max(temp_view_max - TEMP_MIN, 1)
    speed_range = max(SPEED_MAX - SPEED_MIN, 1)

    def temp_to_x(temp: float) -> int:
        return max(0, min(width - 2, int((temp - TEMP_MIN) / temp_range * (width - 2))))

    def speed_to_y(speed: float) -> int:
        return max(0, min(height - 1, int((SPEED_MAX - speed) / speed_range * (height - 1))))

    if state.smooth_enabled:
        for cx in range(width - 1):
            t = TEMP_MIN + cx * temp_range / max(width - 2, 1)
            y = speed_to_y(interpolate(state.temps, state.speeds, t))
            safe_addstr(stdscr, start_row + 1 + y, start_col + 1 + cx, ".")
    else:
        for idx in range(state.num_points - 1):
            x, y = temp_to_x(state.temps[idx]), speed_to_y(state.speeds[idx])
            next_x, next_y = temp_to_x(state.temps[idx + 1]), speed_to_y(state.speeds[idx + 1])
            # Horizontal run, then a vertical step: the classic stepped curve.
            for x_step in range(min(x, next_x), max(x, next_x) + 1):
                safe_addstr(stdscr, start_row + 1 + y, start_col + 1 + x_step, "-")
            direction = 1 if next_y > y else -1
            for y_step in range(y, next_y, direction):
                safe_addstr(stdscr, start_row + 1 + y_step, start_col + 1 + next_x, "|")

    for idx in range(state.num_points):
        x = temp_to_x(state.temps[idx])
        y = speed_to_y(state.speeds[idx])
        char = "*" if idx == state.selected_index and state.focus_area == "points" else "o"
        safe_addstr(stdscr, start_row + 1 + y, start_col + 1 + x, char)

    if live and live[1] >= 0:
        safe_addstr(stdscr, start_row + 1 + speed_to_y(live[1]), start_col + 1 + temp_to_x(live[0]), "@", curses.A_BOLD)


def draw_messages(stdscr: "curses._CursesWindow", height: int, width: int, state: CurveState) -> None:
    if state.error_message:
        stdscr.attron(curses.color_pair(1))
        safe_addstr(stdscr, height - 2, 0, state.error_message[: width - 1].ljust(width - 1))
        stdscr.attroff(curses.color_pair(1))
    else:
        safe_addstr(stdscr, height - 2, 0, " " * (width - 1))
    if state.status_message:
        safe_addstr(stdscr, height - 1, 0, state.status_message[: width - 1].ljust(width - 1))
    else:
        safe_addstr(stdscr, height - 1, 0, " " * (width - 1))


def pick_from_list(stdscr: "curses._CursesWindow", title: str, rows: List[str], current: int,
                   footer: str = " Enter=Select  Esc=Cancel ") -> Optional[int]:
    """Small modal list. Returns the chosen index or None when cancelled."""
    height, width = stdscr.getmaxyx()
    box_width = min(max(max(len(r) for r in rows) + 6, len(footer) + 2, len(title) + 4), max(width - 2, 10))
    box_height = len(rows) + 3
    start_row = max(3, (height - box_height) // 2)
    start_col = max(2, (width - box_width) // 2)

    while True:
        stdscr.attron(curses.A_REVERSE)
        safe_addstr(stdscr, start_row, start_col, f" {title} ".ljust(box_width))
        stdscr.attroff(curses.A_REVERSE)
        for idx, row in enumerate(rows):
            attr = curses.A_REVERSE if idx == current else curses.A_NORMAL
            safe_addstr(stdscr, start_row + 1 + idx, start_col, f"  {row}".ljust(box_width)[:box_width], attr)
        safe_addstr(stdscr, start_row + box_height - 1, start_col, footer.ljust(box_width)[:box_width])
        key = stdscr.getch()
        if key in (curses.KEY_UP, ord("k")):
            current = (current - 1) % len(rows)
        elif key in (curses.KEY_DOWN, ord("j")):
            current = (current + 1) % len(rows)
        elif key in (curses.KEY_ENTER, 10, 13):
            return current
        elif key in (27, ord("q")):
            return None


def prompt_text(stdscr: "curses._CursesWindow", label: str, initial: str) -> Optional[str]:
    """One-line text entry on the bottom line. Enter accepts, Esc cancels."""
    height, width = stdscr.getmaxyx()
    text = initial
    while True:
        line = f"{label}: {text}_"
        safe_addstr(stdscr, height - 2, 0, line[: width - 1].ljust(width - 1), curses.A_REVERSE)
        stdscr.refresh()
        try:
            key = stdscr.get_wch()
        except curses.error:
            continue  # idle tick
        if key in ("\n", "\r") or key == curses.KEY_ENTER:
            return text
        if key == "\x1b":
            return None
        if key in ("\x7f", "\b") or key == curses.KEY_BACKSPACE:
            text = text[:-1]
        elif isinstance(key, str) and key.isprintable() and len(text) < NAME_MAX:
            text += key


def adjust_temperature(state: CurveState, delta: int) -> None:
    idx = state.selected_index
    new_temp = state.temps[idx] + delta
    lower_bound = TEMP_MIN if idx == 0 else state.temps[idx - 1] + 1
    upper_bound = TEMP_MAX if idx == state.num_points - 1 else state.temps[idx + 1] - 1
    if lower_bound > upper_bound:
        return
    new_temp = clamp(new_temp, lower_bound, upper_bound)
    if new_temp != state.temps[idx]:
        state.temps[idx] = new_temp


def adjust_speed(state: CurveState, delta: int) -> None:
    idx = state.selected_index
    new_speed = clamp(state.speeds[idx] + delta, SPEED_MIN, SPEED_MAX)
    if new_speed != state.speeds[idx]:
        state.speeds[idx] = new_speed


def add_point(state: CurveState) -> None:
    if state.num_points >= MAX_POINTS:
        state.error_message = f"Cannot have more than {MAX_POINTS} points."
        return
    idx = state.selected_index
    insert_at = idx + 1
    if insert_at >= state.num_points:
        prev_temp = state.temps[-1]
        if prev_temp >= TEMP_MAX:
            state.error_message = "No room to add another point at high end."
            return
        new_temp = clamp(prev_temp + max(1, (TEMP_MAX - prev_temp) // 2), prev_temp + 1, TEMP_MAX)
        new_speed = state.speeds[-1]
    else:
        prev_temp = state.temps[idx]
        next_temp = state.temps[insert_at]
        if next_temp - prev_temp <= 1:
            state.error_message = "No temperature space between selected points."
            return
        new_temp = prev_temp + (next_temp - prev_temp) // 2
        prev_speed = state.speeds[idx]
        next_speed = state.speeds[insert_at]
        new_speed = clamp(prev_speed + (next_speed - prev_speed) // 2, SPEED_MIN, SPEED_MAX)
    state.temps.insert(insert_at, new_temp)
    state.speeds.insert(insert_at, new_speed)
    state.selected_index = insert_at
    state.error_message = ""


def remove_point(state: CurveState) -> None:
    if state.num_points <= MIN_POINTS:
        state.error_message = f"Curve must have at least {MIN_POINTS} points."
        return
    del state.temps[state.selected_index]
    del state.speeds[state.selected_index]
    if state.selected_index >= state.num_points:
        state.selected_index = state.num_points - 1
    state.error_message = ""


def cycle_focus(state: CurveState) -> None:
    if state.focus_area == "points" and state.focus_axis == 0:
        state.focus_axis = 1
    elif state.focus_area == "points" and state.focus_axis == 1:
        state.focus_area = "fields"
    else:
        state.focus_area = "points"
        state.focus_axis = 0


def update_status(state: CurveState, message: str = "") -> None:
    state.status_message = message


def handle_key(state: CurveState, key: int) -> Tuple[bool, bool]:
    """Handle a key press.

    Returns (should_exit, should_save).
    """
    if key == -1:  # idle tick (live marker refresh)
        return False, False
    state.error_message = ""

    if key in (ord("q"), ord("Q")):
        return True, False
    if key in (ord("s"), ord("S")):
        valid, err = validate_state(state)
        if valid:
            return True, True
        state.error_message = err
        return False, False
    if key in (ord("p"), ord("P")):
        return _handle_profile_pick(state)
    if key in (ord("c"), ord("C")):
        return _handle_profile_copy(state)
    if key in (ord("n"), ord("N")):
        return _handle_profile_rename(state)

    if key in (curses.KEY_BTAB, curses.KEY_STAB, 9):  # TAB
        cycle_focus(state)
        return False, False

    if state.focus_area == "fields":
        return handle_fields_key(state, key)
    return handle_points_key(state, key)


def _profile_rows(state: CurveState, only: Tuple[str, ...] = tuple(ALL_IDS)) -> List[str]:
    rows = []
    for pid in only:
        prof = state.profile if pid == state.profile_id else state.settings.profile(pid)
        kind = "built-in" if prof.builtin else "custom"
        rows.append(f"{prof.name:<{NAME_MAX}} {kind}")
    return rows


def _handle_profile_pick(state: CurveState) -> Tuple[bool, bool]:
    if ACTIVE_SCREEN is None:
        return False, False
    if not state.read_only:
        valid, err = validate_state(state)
        if not valid:
            state.error_message = f"Fix this profile before switching: {err}"
            return False, False
    idx = pick_from_list(ACTIVE_SCREEN, "Profiles (the one you pick runs after Save)", _profile_rows(state),
                         ALL_IDS.index(state.profile_id))
    if idx is not None and ALL_IDS[idx] != state.profile_id:
        state.commit()
        state.view(ALL_IDS[idx])
        state.status_message = (f"{state.profile.name}: built-in, read-only. Press C to copy it into a custom profile."
                                if state.read_only else f"{state.profile.name}: custom profile.")
    return False, False


def _handle_profile_copy(state: CurveState) -> Tuple[bool, bool]:
    if ACTIVE_SCREEN is None:
        return False, False
    valid, err = validate_state(state)
    if not valid:
        state.error_message = err
        return False, False
    targets = tuple(c for c in CUSTOM_IDS if c != state.profile_id)
    idx = pick_from_list(ACTIVE_SCREEN, f"Copy '{state.profile.name}' into (overwrites that slot)",
                         _profile_rows(state, targets), 0)
    if idx is None:
        return False, False
    source = state.profile.copy()
    state.commit()
    dest = state.settings.custom[targets[idx]]
    dest.temps, dest.speeds = list(source.temps), list(source.speeds)
    dest.smooth_enabled, dest.ramp_down, dest.smooth_extra = source.smooth_enabled, source.ramp_down, dict(source.smooth_extra)
    state.view(targets[idx])
    state.status_message = f"Copied into {dest.name}. Rename with N; nothing is saved until S."
    return False, False


def _handle_profile_rename(state: CurveState) -> Tuple[bool, bool]:
    if ACTIVE_SCREEN is None:
        return False, False
    if state.read_only:
        state.error_message = "Built-in profiles keep their names. Copy it to a custom profile first (C)."
        return False, False
    prof = state.settings.custom[state.profile_id]
    new = prompt_text(ACTIVE_SCREEN, f"Name for {state.profile_id} (Enter=ok, Esc=cancel)", prof.name)
    if new is not None:
        prof.name = clean_name(new, f"Custom {state.profile_id[-1]}")
        state.status_message = f"Renamed to {prof.name}."
    return False, False


EDIT_KEYS = (curses.KEY_LEFT, curses.KEY_RIGHT, ord("a"), ord("A"), ord("r"), ord("R"), ord(" "), 10, 13, curses.KEY_ENTER)
READ_ONLY_MSG = "Built-in profile is read-only. Press C to copy it into a custom profile, or P to pick another."


def handle_fields_key(state: CurveState, key: int) -> Tuple[bool, bool]:
    fields = ["gpu_index", "sleep_low", "sleep_high", "sleep_max", "high_temp_threshold", "smooth_enabled", "ramp_down"]
    if key in (curses.KEY_UP, ord("k")):
        state.field_index = (state.field_index - 1) % len(fields)
        return False, False
    if key in (curses.KEY_DOWN, ord("j")):
        state.field_index = (state.field_index + 1) % len(fields)
        return False, False
    attr = fields[state.field_index]
    if state.read_only and attr in ("smooth_enabled", "ramp_down") and key in EDIT_KEYS + (ord("h"), ord("l")):
        state.error_message = READ_ONLY_MSG
        return False, False
    if attr == "ramp_down":
        step = None
        if key in (curses.KEY_LEFT, ord("h")):
            step = -1
        elif key in (curses.KEY_RIGHT, ord("l"), ord(" "), 10, 13, curses.KEY_ENTER):
            step = 1
        if step is not None:
            idx = RAMP_LEVELS.index(state.ramp_down)
            state.ramp_down = RAMP_LEVELS[max(0, min(len(RAMP_LEVELS) - 1, idx + step))] \
                if key not in (ord(" "), 10, 13, curses.KEY_ENTER) else RAMP_LEVELS[(idx + 1) % len(RAMP_LEVELS)]
        return False, False
    if attr == "smooth_enabled":
        if key in (curses.KEY_LEFT, curses.KEY_RIGHT, ord("h"), ord("l"), ord(" "), 10, 13, curses.KEY_ENTER):
            state.smooth_enabled = not state.smooth_enabled
        return False, False
    if key in (curses.KEY_LEFT, ord("h")):
        adjust_runtime_value(state, attr, -1)
    elif key in (curses.KEY_RIGHT, ord("l")):
        adjust_runtime_value(state, attr, 1)
    elif key in (curses.KEY_PPAGE,):
        adjust_runtime_value(state, attr, -5)
    elif key in (curses.KEY_NPAGE,):
        adjust_runtime_value(state, attr, 5)
    return False, False


def adjust_runtime_value(state: CurveState, attr: str, delta: int) -> None:
    value = getattr(state, attr)
    value += delta
    if attr == "gpu_index":
        value = max(0, value)
    elif attr in ("sleep_low", "sleep_high", "sleep_max"):
        value = max(1, value)
    elif attr == "high_temp_threshold":
        value = clamp(value, TEMP_MIN, TEMP_MAX)
    setattr(state, attr, value)


def handle_points_key(state: CurveState, key: int) -> Tuple[bool, bool]:
    if state.read_only and key in EDIT_KEYS:
        state.error_message = READ_ONLY_MSG
        return False, False
    if key in (ord("a"), ord("A")):
        add_point(state)
        return False, False
    if key in (ord("r"), ord("R")):
        remove_point(state)
        return False, False
    if key in (ord(" " ), curses.KEY_ENTER, 10, 13):
        if state.dragging:
            finalize_drag(state)
        else:
            state.dragging = True
            state.drag_origin = state.selected_index
            state.drag_target = state.selected_index
        return False, False
    if state.dragging:
        if key == curses.KEY_UP and state.drag_target > 0:
            state.drag_target -= 1
        elif key == curses.KEY_DOWN and state.drag_target < state.num_points - 1:
            state.drag_target += 1
        return False, False

    if key == curses.KEY_UP:
        state.selected_index = max(0, state.selected_index - 1)
        return False, False
    if key == curses.KEY_DOWN:
        state.selected_index = min(state.num_points - 1, state.selected_index + 1)
        return False, False
    if key == curses.KEY_LEFT:
        if state.focus_axis == 0:
            adjust_temperature(state, -1)
        else:
            adjust_speed(state, -1)
        return False, False
    if key == curses.KEY_RIGHT:
        if state.focus_axis == 0:
            adjust_temperature(state, 1)
        else:
            adjust_speed(state, 1)
        return False, False
    if key in (ord("["),):
        state.focus_axis = 0
        return False, False
    if key in (ord("]"),):
        state.focus_axis = 1
        return False, False
    return False, False


def finalize_drag(state: CurveState) -> None:
    if not state.dragging:
        return
    origin = state.drag_origin
    target = state.drag_target
    if origin != target:
        temp = state.temps.pop(origin)
        speed = state.speeds.pop(origin)
        state.temps.insert(target, temp)
        state.speeds.insert(target, speed)
        state.selected_index = target
    state.dragging = False


def render(stdscr: "curses._CursesWindow", state: CurveState) -> None:
    # erase(), not clear(): clear() forces a full terminal repaint on every
    # refresh (visible flicker); erase() lets curses send only changed cells.
    stdscr.erase()
    height, width = stdscr.getmaxyx()
    draw_top_bar(stdscr, width, state)
    next_row = 4
    next_row = draw_points_table(stdscr, next_row, width // 2, state) + 1
    next_row = draw_run_settings(stdscr, next_row, width // 2, state)
    plot_height = max(10, height - 8)
    plot_width = max(40, width - (width // 2) - 4)
    plot_row = 4
    plot_col = max(width // 2 + 2, 40)
    draw_plot(stdscr, plot_row, plot_col, plot_height - 4, plot_width - 2, state)
    draw_messages(stdscr, height, width, state)
    if state.dragging:
        safe_addstr(stdscr, 3, width // 2, f"Moving point {state.drag_origin} → {state.drag_target}")
    stdscr.refresh()


def curses_main(stdscr: "curses._CursesWindow", state: CurveState) -> Tuple[bool, CurveState]:
    global ACTIVE_SCREEN
    ACTIVE_SCREEN = stdscr
    curses.curs_set(0)
    stdscr.timeout(1000)  # wake once a second to refresh the live marker
    stdscr.keypad(True)
    if curses.has_colors():
        curses.start_color()
        curses.init_pair(1, curses.COLOR_RED, curses.COLOR_BLACK)
    should_exit = False
    should_save = False
    last_live = read_live_status()
    dirty = True
    while not should_exit:
        if dirty:
            render(stdscr, state)
            dirty = False
        key = stdscr.getch()
        if key == -1:
            # Idle tick: only redraw if the live marker actually changed.
            live = read_live_status()
            if live != last_live:
                last_live = live
                dirty = True
            continue
        if key == curses.KEY_RESIZE:
            stdscr.clear()  # one full repaint after a resize
        should_exit, should_save = handle_key(state, key)
        last_live = read_live_status()
        dirty = True
    return should_save, state


def main() -> int:
    parser = argparse.ArgumentParser(description="TUI configurator for NVIDIA Fan Control")
    parser.add_argument("--config", default=CONFIG_PATH, help="Path to configuration file (default: %(default)s)")
    args = parser.parse_args()

    config_path = args.config.strip()
    if not config_path:
        parser.error("--config requires a non-empty path. Set NVFC_CONFIG_PATH or pass an explicit location.")

    state = load_config(config_path)
    curses.stdscr = None  # type: ignore[attr-defined]

    def wrapper(stdscr: "curses._CursesWindow") -> Tuple[bool, CurveState]:
        curses.stdscr = stdscr  # type: ignore[attr-defined]
        return curses_main(stdscr, state)

    should_save, final_state = curses.wrapper(wrapper)
    global ACTIVE_SCREEN
    ACTIVE_SCREEN = None
    if should_save:
        valid, err = validate_state(final_state)
        if not valid:
            print(f"Failed to save: {err}")
            return 1
        try:
            save_config(config_path, final_state)
        except PermissionError:
            parent_cli = os.environ.get("NVFC_PARENT_CLI")
            if parent_cli:
                suggested = parent_cli
            else:
                cli_name = Path(sys.argv[0]).name or "nvfc"
                suggested = shutil.which(cli_name) or cli_name
            cli_name = Path(suggested).name
            print(
                "Failed to save configuration: insufficient permissions to write to "
                f"{config_path}.\n"
                "Re-run the configurator with elevated privileges, for example:\n"
                f"  sudo {suggested} configure"
            )
            if shutil.which(cli_name):
                print(
                    "If sudo still cannot find the command, provide its full path, e.g.:\n"
                    f"  sudo $(command -v {cli_name}) configure"
                )
            return 1
        except OSError as exc:
            print(f"Failed to save configuration: {exc}")
            return 1
        print(f"Configuration saved to {config_path} (active profile: {final_state.profile.name})")
        return 0
    print("No changes saved.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())