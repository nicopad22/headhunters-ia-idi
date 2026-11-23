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
BUILD_ONLY=0
CUSTOM_FLAGS=0
EN_NFC=0
EN_BTN=0
EN_BUZ=0
EN_OLED=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --nfc)     CUSTOM_FLAGS=1; EN_NFC=1; shift ;;
    --buttons) CUSTOM_FLAGS=1; EN_BTN=1; shift ;;
    --buzzer)  CUSTOM_FLAGS=1; EN_BUZ=1; shift ;;
    --oled)    CUSTOM_FLAGS=1; EN_OLED=1; shift ;;
    --all)     CUSTOM_FLAGS=1; EN_NFC=1; EN_BTN=1; EN_BUZ=1; EN_OLED=1; shift ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --port)    PORT="${2:-}"; shift 2 ;;
    -h|--help)
      echo "Usage: ./run_test.sh [--nfc] [--buttons] [--buzzer] [--oled] [--all] [--port /dev/ttyUSB0] [--build-only]"
      echo "Default (no flags): uses include/test_config.h (NFC only)."
      exit 0
      ;;
    *)
      PORT="$1"; shift ;;
  esac
done

if [[ "$CUSTOM_FLAGS" -eq 1 ]]; then
  export PLATFORMIO_BUILD_FLAGS="-DENABLE_TEST_NFC=$EN_NFC -DENABLE_TEST_BUTTONS=$EN_BTN -DENABLE_TEST_BUZZER=$EN_BUZ -DENABLE_TEST_OLED=$EN_OLED"
  echo "==> Custom tests -> NFC:$EN_NFC BUTTONS:$EN_BTN BUZZER:$EN_BUZ OLED:$EN_OLED"
else
  echo "==> Using defaults from include/test_config.h (NFC only)"
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
  echo "1) Plug in your ESP-32D with a DATA USB cable (not charge-only)."
  echo "2) Verify with: ls -l /dev/ttyUSB* /dev/ttyACM*"
  echo "==> Running compile-only check instead..."
  "$PIO_BIN" run
  exit 0
fi

if [[ ! -r "$PORT" || ! -w "$PORT" ]]; then
  echo "Permission denied on $PORT."
  echo "Run once, then log out & back in (or run 'newgrp dialout'):"
  echo "  sudo usermod -aG dialout \"$USER\""
  exit 1
fi

echo "==> Flashing hardware-tests to $PORT..."
"$PIO_BIN" run -t upload --upload-port "$PORT"

echo "==> Opening Serial Monitor at 115200 baud (Ctrl+C to exit)..."
"$PIO_BIN" device monitor --port "$PORT" --baud 115200
