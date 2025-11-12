#!/usr/bin/env bash
set -euo pipefail
DIR="${NVML_MOCK_DIR:-/tmp/nvml-mock}"
mkdir -p "$DIR"

case "${1:-}" in
  set-temp)
    : "${2:?Usage: $0 set-temp <Celsius>}"
    echo "$2" > "$DIR/temperature"
    echo "temperature set to $2°C"
    ;;
  set-fans)
    : "${2:?Usage: $0 set-fans <count>}"
    echo "$2" > "$DIR/fans"
    echo "fans set to $2"
    ;;
  tail-log)
    tail -f "$DIR/fan_speed.log"
    ;;
  *)
    echo "Usage:"
    echo "  $0 set-temp <C>"
    echo "  $0 set-fans <count>"
    echo "  $0 tail-log"
    ;;
esac
