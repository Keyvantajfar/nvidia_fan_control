#!/usr/bin/env python3
"""Prepare a build copy of nvidia_fan_controlV2.c that includes the generated header.

The original source file should never be modified in-place.  This helper copies
nvidia_fan_controlV2.c into the build directory and injects an #include for the
fan curve header while stripping the hard-coded arrays and macros so the build
links against the generated configuration instead.
"""
from __future__ import annotations

import argparse
from pathlib import Path

DEFAULT_SOURCE = "nvidia_fan_controlV2.c"
DEFAULT_OUTPUT = "build/nvidia_fan_controlV2.c"
DEFAULT_HEADER = "fan_curve_config.h"


REMOVED_MACROS = {
    "#define NUM_POINTS",
    "#define SLEEP_LOW",
    "#define SLEEP_HIGH",
    "#define HIGH_TEMP_THRESHOLD",
}

REMOVED_DECLS = {
    "int temperature_points",
    "int fan_speed_points",
}


def process_source(source: Path, header: str) -> str:
    text = source.read_text()
    lines = text.splitlines()

    filtered_lines = []
    for line in lines:
        stripped = line.strip()
        if any(stripped.startswith(prefix) for prefix in REMOVED_MACROS):
            continue
        if any(prefix in stripped for prefix in REMOVED_DECLS):
            continue
        if "fan_curve_config.h" in stripped:
            continue
        filtered_lines.append(line)

    # Identify last include to inject after
    insert_idx = 0
    for idx, line in enumerate(filtered_lines):
        if line.strip().startswith("#include"):
            insert_idx = idx
    filtered_lines.insert(insert_idx + 1, f'#include "{header}"')
    return "\n".join(filtered_lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Copy and patch the daemon source for build")
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Original C source path (default: %(default)s)")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="Patched C output path (default: %(default)s)")
    parser.add_argument("--header", default=DEFAULT_HEADER, help="Header include relative path (default: %(default)s)")
    args = parser.parse_args()

    source = Path(args.source)
    output = Path(args.output)
    if not source.exists():
        raise SystemExit(f"Source file not found: {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    patched = process_source(source, Path(args.header).name)
    output.write_text(patched)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
