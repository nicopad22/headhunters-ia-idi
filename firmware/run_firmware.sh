#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PIO_BIN="$(command -v pio || echo "$HOME/.local/bin/pio")"
if [[ ! -x "$PIO_BIN" ]]; then
  echo "ERROR: PlatformIO ('pio') not found. Install it with:"
  echo "  uv tool install --python python3.12 platformio"
  exit 1
fi

PORT=""
DUMMY_TEST=0
BUILD_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dummy-test) DUMMY_TEST=1; shift ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --port)       PORT="${2:-}"; shift 2 ;;
    -h|--help)
      echo "Usage: ./run_firmware.sh [--dummy-test] [--port /dev/ttyUSB0] [--build-only]"
      echo "  (default)     Flashes production game firmware & opens Serial Monitor"
      echo "  --dummy-test  Flashes guided 6-card, 11-step full-system flow test"
      exit 0
      ;;
    *) PORT="$1"; shift ;;
  esac
done

if [[ "$DUMMY_TEST" -eq 1 ]]; then
  export PLATFORMIO_BUILD_FLAGS="-DDUMMY_FLOW_TEST=1"
  echo "==> Mode: GUIDED DUMMY FLOW TEST (6 cards, 11 steps)"
else
  echo "==> Mode: PRODUCTION GAME FIRMWARE"
fi

if [[ "$BUILD_ONLY" -eq 1 ]]; then
  "$PIO_BIN" run
  exit 0
fi

if [[ -z "$PORT" ]]; then
  PORT="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n 1 || true)"
fi

if [[ -z "$PORT" ]]; then
  echo "No ESP32 serial port found (/dev/ttyUSB* or /dev/ttyACM*)."
  echo "Plug in your ESP-32D, then re-run this script."
  exit 1
fi

echo "==> Flashing firmware to $PORT..."
"$PIO_BIN" run -t upload --upload-port "$PORT"

echo "==> Opening Serial Monitor at 115200 baud (Ctrl+C to exit)..."
"$PIO_BIN" device monitor --port "$PORT" --baud 115200
