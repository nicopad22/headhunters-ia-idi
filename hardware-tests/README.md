# Hardware Diagnostics — Headhunters ESP-32D

Standalone PlatformIO test project using the exact same pins as `../firmware/include/pin_config.h`.
By default, **only RC522 NFC (`ENABLE_TEST_NFC`) is enabled** in `include/test_config.h`.

## Quick Start

```bash
cd hardware-tests
./run_test.sh              # Default: NFC (RC522) only — flashes & opens Serial Monitor
```

## Run Individual Component Tests

Either pass CLI flags to `./run_test.sh`, or flip `0`/`1` inside `include/test_config.h`:

```bash
./run_test.sh --nfc        # RC522 VersionReg, self-test, UID + SAK + unique card tracker
./run_test.sh --buttons    # 5 buttons (GPIO 32, 33, 25, 26, 27) stuck check + debounced press logs
./run_test.sh --buzzer     # Passive buzzer (GPIO 14) startup scale + OK/ERROR/WIN tones
./run_test.sh --oled       # I2C scanner (SDA=21, SCL=22) + SH1106 128x64 animated progress bars
./run_test.sh --nfc --oled # Combine tests (tapped card UIDs also show on OLED)
./run_test.sh --all        # Run all 4 tests together
```

## If Upload Fails on Fedora
- **Permission denied on `/dev/ttyUSB0`:** Run `sudo usermod -aG dialout "$USER"` once, then log out and back in (or run `newgrp dialout`).
- **Stuck at `Connecting........___`:** Hold the **BOOT** button on the ESP-32D when `Connecting...` appears and release once uploading starts.
