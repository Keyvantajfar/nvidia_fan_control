#!/usr/bin/env python3
"""Generate fan curve header from an INI configuration.

This script reads /etc/nvidia-fan-controlV2.conf (or a provided path) and emits
build/fan_curve_config.h suitable for inclusion by the daemon during
compilation.  The header mirrors the values specified in the configuration so
that the compiled binary reflects the saved fan curve and runtime parameters.
"""
from __future__ import annotations

import argparse
import configparser
from pathlib import Path
from typing import List

CONFIG_PATH = "/etc/nvidia-fan-controlV2.conf"
HEADER_PATH = "build/fan_curve_config.h"

MIN_POINTS = 3
MAX_POINTS = 7
TEMP_MIN = 0
TEMP_MAX = 100
SPEED_MIN = 0
SPEED_MAX = 100


def parse_list(raw: str) -> List[int]:
    if not raw.strip():
        return []
    return [int(item.strip()) for item in raw.split(",") if item.strip()]


def validate(temps: List[int], speeds: List[int]) -> None:
    if not (MIN_POINTS <= len(temps) <= MAX_POINTS):
        raise ValueError(f"Expected between {MIN_POINTS} and {MAX_POINTS} temperature points, got {len(temps)}")
    if len(temps) != len(speeds):
        raise ValueError("Temperature and speed lists must have the same length")
    last_temp = -1
    for idx, temp in enumerate(temps):
        if not (TEMP_MIN <= temp <= TEMP_MAX):
            raise ValueError(f"Temperature at index {idx} out of range: {temp}")
        if temp <= last_temp:
            raise ValueError("Temperatures must be strictly increasing")
        last_temp = temp
    for idx, speed in enumerate(speeds):
        if not (SPEED_MIN <= speed <= SPEED_MAX):
            raise ValueError(f"Speed at index {idx} out of range: {speed}")


def write_header(path: Path, temps: List[int], speeds: List[int], sleep_low: int, sleep_high: int, high_temp_threshold: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    contents = [
        "#pragma once\n",
        f"#define NUM_POINTS {len(temps)}\n",
        f"static int temperature_points[NUM_POINTS] = {{{', '.join(str(x) for x in temps)}}};\n",
        f"static int fan_speed_points[NUM_POINTS]   = {{{', '.join(str(x) for x in speeds)}}};\n",
        f"#define SLEEP_LOW {sleep_low}\n",
        f"#define SLEEP_HIGH {sleep_high}\n",
        f"#define HIGH_TEMP_THRESHOLD {high_temp_threshold}\n",
    ]
    path.write_text("".join(contents), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate fan_curve_config.h from configuration")
    parser.add_argument("--config", default=CONFIG_PATH, help="Path to configuration file (default: %(default)s)")
    parser.add_argument("--output", default=HEADER_PATH, help="Header output path (default: %(default)s)")
    args = parser.parse_args()

    config_path = Path(args.config)
    if not config_path.exists():
        raise SystemExit(f"Configuration file not found: {config_path}")

    parser_cfg = configparser.ConfigParser()
    parser_cfg.read(config_path)

    try:
        temps = parse_list(parser_cfg.get("curve", "temps"))
        speeds = parse_list(parser_cfg.get("curve", "speeds"))
    except configparser.Error as exc:
        raise SystemExit(f"Invalid configuration: {exc}")

    try:
        validate(temps, speeds)
    except ValueError as exc:
        raise SystemExit(str(exc))

    sleep_low = parser_cfg.getint("run", "sleep_low", fallback=5)
    sleep_high = parser_cfg.getint("run", "sleep_high", fallback=2)
    high_temp_threshold = parser_cfg.getint("run", "high_temp_threshold", fallback=42)

    write_header(Path(args.output), temps, speeds, sleep_low, sleep_high, high_temp_threshold)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
