#!/usr/bin/env python3.12
"""Interactive NFC card enrollment tool for Headhunters (IDI1015).

Reads scanned UIDs directly over USB Serial from the ESP-32D (works with either
hardware-tests or production firmware flashed), rejects duplicate cards, saves
data/game_data.json after every tap, and regenerates firmware/include/game_data.h.

Usage:
  python3.12 scripts/enroll_cards.py                  # Enroll all unassigned cards
  python3.12 scripts/enroll_cards.py --status         # Show enrollment progress
  python3.12 scripts/enroll_cards.py --group candidates
  python3.12 scripts/enroll_cards.py --group occupations
  python3.12 scripts/enroll_cards.py --group mystery
  python3.12 scripts/enroll_cards.py --port /dev/ttyUSB0
  python3.12 scripts/enroll_cards.py --pull           # Copy UIDs assigned on the board (admin mode) into the JSON
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA_PATH = ROOT / "data" / "game_data.json"
GEN_SCRIPT = ROOT / "scripts" / "generate_header.py"

# Matches both hardware-tests (`nfc_uid: "AABBCCDD"`) and firmware (`UID escaneado: 0xAABBCCDD`)
UID_PATTERNS = [
    re.compile(r'nfc_uid:\s*"([0-9A-Fa-f]{8})"'),
    re.compile(r'UID escaneado:\s*0x([0-9A-Fa-f]{8})'),
]


def detect_port() -> str | None:
    ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def open_serial_posix(port: str, baud: int = 115200):
    """Open a Linux serial device at 115200 8N1 using stdlib termios (no pyserial required)."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    # Raw input, 8N1, no flow control
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)
    return os.fdopen(fd, "rb", buffering=0)


def read_next_uid(ser) -> str:
    buf = bytearray()
    while True:
        ch = ser.read(1)
        if not ch:
            continue
        if ch in (b"\n", b"\r"):
            if buf:
                line = buf.decode("utf-8", errors="ignore").strip()
                buf.clear()
                for pat in UID_PATTERNS:
                    m = pat.search(line)
                    if m:
                        return m.group(1).upper()
        else:
            buf.extend(ch)


def pull_from_board(port: str, data: dict) -> None:
    """Asks the production firmware for its flash mapping ('dump') and writes it to game_data.json."""
    by_code = {code: obj for code, _, obj in build_items(data, "all")}
    dump_re = re.compile(r"^CARD ([CIM]\d{2}) ([0-9A-Fa-f]{8})$")
    with open_serial_posix(port) as ser:
        fd = ser.fileno()
        time.sleep(2.5)  # opening the port may reset the ESP32; wait for boot
        termios.tcflush(fd, termios.TCIFLUSH)
        os.write(fd, b"dump\n")
        buf, lines, deadline = bytearray(), [], time.time() + 6
        done = False
        while time.time() < deadline and not done:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                continue
            buf.extend(os.read(fd, 256))
            while b"\n" in buf:
                raw, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                line = raw.decode("utf-8", errors="ignore").strip()
                if line == "END DUMP":
                    done = True
                    break
                m = dump_re.match(line)
                if m:
                    lines.append((m.group(1), m.group(2).upper()))
    if not done:
        print("ERROR: no dump received. Is the production firmware flashed and the Serial Monitor closed?")
        sys.exit(1)
    changed = 0
    for code, uid in lines:
        obj = by_code.get(code)
        if obj is None:
            continue
        new_uid = "" if uid == "00000000" else uid
        if (obj.get("nfc_uid") or "").upper() != new_uid:
            obj["nfc_uid"] = new_uid
            changed += 1
    assigned = sum(1 for _, uid in lines if uid != "00000000")
    print(f"Board reports {assigned}/{len(lines)} cards assigned; {changed} changed in game_data.json.")
    save_and_generate(data)


def build_items(data: dict, group: str) -> list[tuple[str, str, dict]]:
    occ_by_id = {o["id"]: o for o in data["occupations"]}
    items: list[tuple[str, str, dict]] = []

    if group in ("all", "candidates"):
        for i, c in enumerate(data["candidates"], start=1):
            occ = occ_by_id[c["occupation_id"]]
            label = (
                f"CANDIDATE C{i:02d} | {occ['cargo']} (Sala {occ['room_code']}) | "
                f"{c['modality']} (${c['cost']}) | {c['name']}"
            )
            items.append((f"C{i:02d}", label, c))

    if group in ("all", "occupations"):
        for i, o in enumerate(data["occupations"], start=1):
            label = f"INSIGHT   I{i:02d} | {o['cargo']} (Sala {o['room_code']}, OIT {o['oit_score']:.2f})"
            items.append((f"I{i:02d}", label, o))

    if group in ("all", "mystery"):
        for i, m in enumerate(data["mystery_cards"], start=1):
            label = f"MYSTERY   M{i:02d} | #{m['id']:02d} {m['title']} ({m['effect']})"
            items.append((f"M{i:02d}", label, m))

    return items


def existing_uid_map(data: dict) -> dict[str, str]:
    mapping: dict[str, str] = {}
    for code, label, obj in build_items(data, "all"):
        uid = (obj.get("nfc_uid") or "").strip().upper()
        if uid:
            mapping[uid] = f"{code}: {label}"
    return mapping


def save_and_generate(data: dict) -> None:
    DATA_PATH.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run([sys.executable, str(GEN_SCRIPT)], check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Enroll the 50 NFC cards into data/game_data.json.")
    parser.add_argument("--group", choices=("all", "candidates", "occupations", "mystery"), default="all")
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect /dev/ttyUSB*)")
    parser.add_argument("--status", action="store_true", help="Print enrollment progress and exit")
    parser.add_argument("--reset", action="store_true", help="Clear existing UIDs in the selected group first")
    parser.add_argument("--pull", action="store_true",
                        help="Read the UIDs assigned on the board (admin mode) into game_data.json")
    args = parser.parse_args()

    data = json.loads(DATA_PATH.read_text(encoding="utf-8"))
    items = build_items(data, args.group)

    if args.status:
        done = sum(1 for _, _, o in items if (o.get("nfc_uid") or "").strip())
        print(f"Enrolled ({args.group}): {done}/{len(items)}")
        for code, label, obj in items:
            uid = (obj.get("nfc_uid") or "").strip()
            mark = f"[{uid}]" if uid else "[  UNASSIGNED  ]"
            print(f"  {mark}  {label}")
        return

    if args.pull:
        port = args.port or detect_port()
        if not port:
            print("ERROR: No ESP32 serial port found (/dev/ttyUSB* or /dev/ttyACM*).")
            sys.exit(1)
        pull_from_board(port, data)
        return

    if args.reset:
        for _, _, obj in items:
            obj["nfc_uid"] = ""
        DATA_PATH.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Cleared UIDs for group '{args.group}'.")

    pending = [(code, label, obj) for code, label, obj in items if not (obj.get("nfc_uid") or "").strip()]
    if not pending:
        print(f"All {len(items)} cards in '{args.group}' are already enrolled! Use --reset to redo.")
        return

    port = args.port or detect_port()
    if not port:
        print("ERROR: No ESP32 serial port found (/dev/ttyUSB* or /dev/ttyACM*).")
        print("Plug in your ESP-32D (flashed with hardware-tests or firmware) and retry.")
        sys.exit(1)

    print(f"Connected to {port} (115200 baud). Close PlatformIO Serial Monitor first if it's open!")
    print(f"Cards remaining in '{args.group}': {len(pending)}/{len(items)}")
    print("Press Ctrl+C at any time to stop safely (progress is saved after every card).\n")

    used_uids = existing_uid_map(data)
    enrolled_now = 0

    try:
        with open_serial_posix(port) as ser:
            for idx, (code, label, obj) in enumerate(pending, start=1):
                print(f"[{idx}/{len(pending)}] {label}")
                print(f"    -> Tap the physical NFC card for {code} now...")
                while True:
                    uid = read_next_uid(ser)
                    if uid in used_uids:
                        print(f"    [DUPLICATE!] 0x{uid} is already assigned to: {used_uids[uid]}")
                        print(f"    -> Tap a DIFFERENT physical card for {code}...")
                        continue
                    obj["nfc_uid"] = uid
                    used_uids[uid] = f"{code}: {label}"
                    DATA_PATH.write_text(
                        json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
                    )
                    enrolled_now += 1
                    print(f"    [SAVED] {code} = 0x{uid}  (mark card '{code}' if blank!)\n")
                    break
    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        if enrolled_now > 0:
            print(f"Enrolled {enrolled_now} card(s). Regenerating firmware/include/game_data.h...")
            save_and_generate(data)
            print("Done! Flash updated firmware with: cd firmware && ./run_firmware.sh")


if __name__ == "__main__":
    main()
