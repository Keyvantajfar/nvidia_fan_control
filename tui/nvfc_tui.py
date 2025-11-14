#!/usr/bin/env python3
"""Curses-based configurator for nvidia_fan_controlV2.

This module implements the interactive terminal user interface for defining the
fan curve and runtime parameters that will be embedded into the daemon at
install time.  The UI is intentionally dependency-free (only the Python stdlib)
and can run directly on a console or over SSH.

Key bindings (also shown in the UI):
    ↑/↓   Move between curve points (or move a grabbed point)
    ←/→   Adjust the currently focused value (temperature or speed)
    TAB   Cycle focus between temperature, speed, and runtime parameter panes
    SPACE Pick up / drop the current point for reordering
    A/R   Add or remove a point (between 3 and 7 points)
    T     Choose a preset template (Quiet / Default / Aggressive / Custom)
    S     Save
    Q     Quit without saving

The TUI persists configuration to /etc/nvidia-fan-controlV2.conf using a simple
INI structure that other tooling in this project consumes.
"""
from __future__ import annotations

import argparse
import configparser
import curses
import os
from dataclasses import dataclass
from typing import Dict, List, Tuple

ACTIVE_SCREEN = None

def safe_addstr(win, y, x, text, attr=None):
    try:
        if attr is None:
            win.addstr(y, x, text)
        else:
            win.addstr(y, x, text, attr)
    except curses.error:
        pass



CONFIG_PATH = "/etc/nvidia-fan-controlV2.conf"
MIN_POINTS = 3
MAX_POINTS = 7
TEMP_MIN = 0
TEMP_MAX = 100
SPEED_MIN = 0
SPEED_MAX = 100

DEFAULT_RUN_SETTINGS = {
    "gpu_index": 0,
    "sleep_low": 5,
    "sleep_high": 2,
    "high_temp_threshold": 42,
}

TEMPLATES: Dict[str, Tuple[List[int], List[int]]] = {
    "Quiet": ([0, 45, 60, 72, 82, 92, 100], [15, 20, 30, 45, 60, 75, 90]),
    "Default": ([0, 40, 55, 67, 75, 85], [25, 30, 45, 65, 78, 99]),
    "Aggressive": ([0, 35, 45, 55, 65, 75, 85], [30, 40, 55, 70, 85, 95, 100]),
    "Custom": (None, None),  # Filled with current values when selected
}


@dataclass
class CurveState:
    temps: List[int]
    speeds: List[int]
    gpu_index: int = DEFAULT_RUN_SETTINGS["gpu_index"]
    sleep_low: int = DEFAULT_RUN_SETTINGS["sleep_low"]
    sleep_high: int = DEFAULT_RUN_SETTINGS["sleep_high"]
    high_temp_threshold: int = DEFAULT_RUN_SETTINGS["high_temp_threshold"]
    selected_index: int = 0
    focus_axis: int = 0  # 0 -> temperature, 1 -> speed
    focus_area: str = "points"  # "points" or "fields"
    field_index: int = 0
    dragging: bool = False
    drag_origin: int = 0
    drag_target: int = 0
    preset_name: str = "Custom"
    modified_since_preset: bool = False
    status_message: str = ""
    error_message: str = ""

    def clone(self) -> "CurveState":
        return CurveState(
            temps=list(self.temps),
            speeds=list(self.speeds),
            gpu_index=self.gpu_index,
            sleep_low=self.sleep_low,
            sleep_high=self.sleep_high,
            high_temp_threshold=self.high_temp_threshold,
            selected_index=self.selected_index,
            focus_axis=self.focus_axis,
            focus_area=self.focus_area,
            field_index=self.field_index,
            dragging=self.dragging,
            drag_origin=self.drag_origin,
            drag_target=self.drag_target,
            preset_name=self.preset_name,
            modified_since_preset=self.modified_since_preset,
            status_message=self.status_message,
            error_message=self.error_message,
        )

    @property
    def num_points(self) -> int:
        return len(self.temps)

    def mark_modified(self) -> None:
        if self.preset_name != "Custom":
            self.modified_since_preset = True

    def ensure_custom_preset(self) -> None:
        if self.preset_name != "Custom" and self.modified_since_preset:
            self.preset_name = "Custom"


def load_config(path: str) -> CurveState:
    if not os.path.exists(path):
        temps, speeds = TEMPLATES["Default"]
        return CurveState(list(temps), list(speeds))

    parser = configparser.ConfigParser()
    parser.read(path)

    temps_raw = parser.get("curve", "temps", fallback="")
    speeds_raw = parser.get("curve", "speeds", fallback="")

    temps = [int(x.strip()) for x in temps_raw.split(",") if x.strip()]
    speeds = [int(x.strip()) for x in speeds_raw.split(",") if x.strip()]

    state = CurveState(temps, speeds)
    state.gpu_index = parser.getint("run", "gpu_index", fallback=DEFAULT_RUN_SETTINGS["gpu_index"])
    state.sleep_low = parser.getint("run", "sleep_low", fallback=DEFAULT_RUN_SETTINGS["sleep_low"])
    state.sleep_high = parser.getint("run", "sleep_high", fallback=DEFAULT_RUN_SETTINGS["sleep_high"])
    state.high_temp_threshold = parser.getint(
        "run", "high_temp_threshold", fallback=DEFAULT_RUN_SETTINGS["high_temp_threshold"]
    )
    # Try to detect matching template
    for name, (temps_template, speeds_template) in TEMPLATES.items():
        if temps_template is None:
            continue
        if temps == list(temps_template) and speeds == list(speeds_template):
            state.preset_name = name
            break
    else:
        state.preset_name = "Custom"
    return state


def save_config(path: str, state: CurveState) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    parser = configparser.ConfigParser()
    parser["curve"] = {
        "temps": ",".join(str(x) for x in state.temps),
        "speeds": ",".join(str(x) for x in state.speeds),
    }
    parser["run"] = {
        "gpu_index": str(state.gpu_index),
        "sleep_low": str(state.sleep_low),
        "sleep_high": str(state.sleep_high),
        "high_temp_threshold": str(state.high_temp_threshold),
    }
    with open(path, "w", encoding="utf-8") as fp:
        parser.write(fp)


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, value))


def validate_state(state: CurveState) -> Tuple[bool, str]:
    if not (MIN_POINTS <= state.num_points <= MAX_POINTS):
        return False, f"Curve must contain between {MIN_POINTS} and {MAX_POINTS} points."
    if len(state.temps) != len(state.speeds):
        return False, "Temperature and speed point counts must match."
    last_temp = -1
    for idx, temp in enumerate(state.temps):
        if not (TEMP_MIN <= temp <= TEMP_MAX):
            return False, f"Temperature at index {idx} must be within {TEMP_MIN}-{TEMP_MAX}."
        if temp <= last_temp:
            return False, "Temperatures must be strictly increasing."
        last_temp = temp
    for idx, speed in enumerate(state.speeds):
        if not (SPEED_MIN <= speed <= SPEED_MAX):
            return False, f"Speed at index {idx} must be within {SPEED_MIN}-{SPEED_MAX}."
    if state.sleep_low < 1 or state.sleep_high < 1:
        return False, "Sleep intervals must be positive integers."
    if state.high_temp_threshold < TEMP_MIN or state.high_temp_threshold > TEMP_MAX:
        return False, "High temperature threshold must fall within 0-100°C."
    if state.gpu_index < 0:
        return False, "GPU index cannot be negative."
    return True, ""


def draw_top_bar(stdscr: "curses._CursesWindow", width: int, state: CurveState) -> None:
    title = " NVIDIA Fan Control V2 Configurator "
    hints = "↑/↓ move  ←/→ adjust  SPACE grab  TAB switch focus  A add  R remove  T presets  S save  Q quit"
    stdscr.attron(curses.A_REVERSE)
    safe_addstr(stdscr, 0, 0, title.ljust(width))
    stdscr.attroff(curses.A_REVERSE)
    safe_addstr(stdscr, 1, 0, hints.ljust(width))
    safe_addstr(stdscr, 2, 0, f"Preset: {state.preset_name} {'(modified)' if state.modified_since_preset else ''}".ljust(width))


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
        ("High temp threshold", state.high_temp_threshold),
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
    safe_addstr(stdscr, start_row + height + 1, start_col, "Temp (°C)")
    safe_addstr(stdscr, start_row - 1, start_col, "Speed (%)")

    if not state.temps:
        return

    temp_range = max(TEMP_MAX - TEMP_MIN, 1)
    speed_range = max(SPEED_MAX - SPEED_MIN, 1)

    def temp_to_x(temp: int) -> int:
        return int((temp - TEMP_MIN) / temp_range * (width - 2))

    def speed_to_y(speed: int) -> int:
        return int((SPEED_MAX - speed) / speed_range * (height - 1))

    for idx in range(state.num_points):
        temp = state.temps[idx]
        speed = state.speeds[idx]
        x = temp_to_x(temp)
        y = speed_to_y(speed)
        char = "*" if idx == state.selected_index and state.focus_area == "points" else "o"
        safe_addstr(stdscr, start_row + 1 + y, start_col + 1 + x, char)
        if idx < state.num_points - 1:
            next_temp = state.temps[idx + 1]
            next_speed = state.speeds[idx + 1]
            next_x = temp_to_x(next_temp)
            next_y = speed_to_y(next_speed)
            # Draw horizontal line then vertical step to emulate step curve
            for x_step in range(min(x, next_x), max(x, next_x) + 1):
                safe_addstr(stdscr, start_row + 1 + y, start_col + 1 + x_step, "-")
            direction = 1 if next_y > y else -1
            for y_step in range(y, next_y, direction):
                safe_addstr(stdscr, start_row + 1 + y_step, start_col + 1 + next_x, "|")


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


def select_template(stdscr: "curses._CursesWindow", state: CurveState) -> Tuple[List[int], List[int], str]:
    options = list(TEMPLATES.keys())
    current = options.index(state.preset_name) if state.preset_name in options else 0

    height, width = stdscr.getmaxyx()
    box_width = max(len(opt) for opt in options) + 8
    box_height = len(options) + 4
    start_row = max(3, (height - box_height) // 2)
    start_col = max(2, (width - box_width) // 2)

    while True:
        stdscr.attron(curses.A_REVERSE)
        safe_addstr(stdscr, start_row, start_col, " Presets ".ljust(box_width))
        stdscr.attroff(curses.A_REVERSE)
        for idx, opt in enumerate(options):
            attr = curses.A_REVERSE if idx == current else curses.A_NORMAL
            safe_addstr(stdscr, start_row + 1 + idx, start_col, f"  {opt}".ljust(box_width), attr)
        safe_addstr(stdscr, start_row + box_height - 1, start_col, " Enter=Select  Esc=Cancel ".ljust(box_width))
        key = stdscr.getch()
        if key in (curses.KEY_UP, ord("k")):
            current = (current - 1) % len(options)
        elif key in (curses.KEY_DOWN, ord("j")):
            current = (current + 1) % len(options)
        elif key in (curses.KEY_ENTER, 10, 13):
            name = options[current]
            temps, speeds = TEMPLATES[name]
            if temps is None:
                return state.temps, state.speeds, "Custom"
            return list(temps), list(speeds), name
        elif key in (27, ord("q")):
            return state.temps, state.speeds, state.preset_name


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
        state.mark_modified()
        state.ensure_custom_preset()


def adjust_speed(state: CurveState, delta: int) -> None:
    idx = state.selected_index
    new_speed = clamp(state.speeds[idx] + delta, SPEED_MIN, SPEED_MAX)
    if new_speed != state.speeds[idx]:
        state.speeds[idx] = new_speed
        state.mark_modified()
        state.ensure_custom_preset()


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
    state.mark_modified()
    state.ensure_custom_preset()
    state.error_message = ""


def remove_point(state: CurveState) -> None:
    if state.num_points <= MIN_POINTS:
        state.error_message = f"Curve must have at least {MIN_POINTS} points."
        return
    del state.temps[state.selected_index]
    del state.speeds[state.selected_index]
    if state.selected_index >= state.num_points:
        state.selected_index = state.num_points - 1
    state.mark_modified()
    state.ensure_custom_preset()
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
    state.error_message = ""

    if key in (ord("q"), ord("Q")):
        return True, False
    if key in (ord("s"), ord("S")):
        valid, err = validate_state(state)
        if valid:
            return True, True
        state.error_message = err
        return False, False
    if key in (ord("t"), ord("T")):
        return _handle_template_selection(state)

    if key in (curses.KEY_BTAB, curses.KEY_STAB, 9):  # TAB
        cycle_focus(state)
        return False, False

    if state.focus_area == "fields":
        return handle_fields_key(state, key)
    return handle_points_key(state, key)


def _handle_template_selection(state: CurveState) -> Tuple[bool, bool]:
    global ACTIVE_SCREEN
    if ACTIVE_SCREEN is None:
        return False, False
    temps, speeds, name = select_template(ACTIVE_SCREEN, state)
    if name != state.preset_name:
        state.modified_since_preset = False
    state.preset_name = name
    state.temps = list(temps)
    state.speeds = list(speeds)
    state.selected_index = min(state.selected_index, state.num_points - 1)
    state.mark_modified()
    if name != "Custom":
        state.modified_since_preset = False
    return False, False


def handle_fields_key(state: CurveState, key: int) -> Tuple[bool, bool]:
    fields = ["gpu_index", "sleep_low", "sleep_high", "high_temp_threshold"]
    if key in (curses.KEY_UP, ord("k")):
        state.field_index = (state.field_index - 1) % len(fields)
        return False, False
    if key in (curses.KEY_DOWN, ord("j")):
        state.field_index = (state.field_index + 1) % len(fields)
        return False, False
    attr = fields[state.field_index]
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
    elif attr in ("sleep_low", "sleep_high"):
        value = max(1, value)
    elif attr == "high_temp_threshold":
        value = clamp(value, TEMP_MIN, TEMP_MAX)
    setattr(state, attr, value)
    state.mark_modified()
    state.ensure_custom_preset()


def handle_points_key(state: CurveState, key: int) -> Tuple[bool, bool]:
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
        state.mark_modified()
        state.ensure_custom_preset()
    state.dragging = False


def render(stdscr: "curses._CursesWindow", state: CurveState) -> None:
    stdscr.clear()
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
    stdscr.nodelay(False)
    stdscr.keypad(True)
    if curses.has_colors():
        curses.start_color()
        curses.init_pair(1, curses.COLOR_RED, curses.COLOR_BLACK)
    should_exit = False
    should_save = False
    while not should_exit:
        render(stdscr, state)
        key = stdscr.getch()
        should_exit, should_save = handle_key(state, key)
    return should_save, state


def main() -> int:
    parser = argparse.ArgumentParser(description="TUI configurator for nvidia-fan-controlV2")
    parser.add_argument("--config", default=CONFIG_PATH, help="Path to configuration file (default: %(default)s)")
    args = parser.parse_args()

    state = load_config(args.config)
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
        save_config(args.config, final_state)
        print(f"Configuration saved to {args.config}")
        return 0
    print("No changes saved.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
