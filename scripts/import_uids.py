#!/usr/bin/env python3.12
"""Import the JSON exported by the phone web app (webapp/) into data/game_data.json.

Then regenerates firmware/include/game_data.h (and webapp/cards.json). Flash the
firmware afterwards and the board uses these UIDs: a newly imported card table
takes priority over anything previously saved on the board in admin mode.

Usage:
  python3.12 scripts/import_uids.py ~/Descargas/headhunters_nfc_uids.json
  python3.12 scripts/import_uids.py export.json --dry-run    # show changes only
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA_PATH = ROOT / "data" / "game_data.json"
GEN_SCRIPT = ROOT / "scripts" / "generate_header.py"
HEX8 = re.compile(r"^[0-9A-F]{8}$")


def items_by_code(data: dict) -> dict[str, tuple[dict, str]]:
    """Same codes/order as enroll_cards.py and the firmware: C01..C24, I01..I08, M01..M18"""
    out: dict[str, tuple[dict, str]] = {}
    for i, c in enumerate(data["candidates"], start=1):
        out[f"C{i:02d}"] = (c, c["name"])
    for i, o in enumerate(data["occupations"], start=1):
        out[f"I{i:02d}"] = (o, f"Insight {o['cargo']}")
    for i, m in enumerate(data["mystery_cards"], start=1):
        out[f"M{i:02d}"] = (m, f"Misterio #{m['id']:02d} {m['title']}")
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("export_json", type=Path, help="File downloaded from the web app")
    parser.add_argument("--dry-run", action="store_true", help="Print what would change, write nothing")
    args = parser.parse_args()

    export = json.loads(args.export_json.read_text(encoding="utf-8"))
    if export.get("format") != "headhunters-nfc-uids" or not isinstance(export.get("cards"), list):
        sys.exit("ERROR: this file is not an export from the Headhunters NFC web app.")

    data = json.loads(DATA_PATH.read_text(encoding="utf-8"))
    by_code = items_by_code(data)

    incoming: dict[str, str] = {}
    errors: list[str] = []
    for card in export["cards"]:
        code = str(card.get("code", ""))
        uid = str(card.get("uid", "")).replace(":", "").strip().upper()
        if code not in by_code:
            errors.append(f"unknown card code {code!r}")
            continue
        if uid and not HEX8.match(uid):
            errors.append(f"{code}: invalid uid {uid!r} (expected 8 hex digits)")
            continue
        incoming[code] = uid

    seen: dict[str, str] = {}
    for code, uid in incoming.items():
        if uid and uid in seen:
            errors.append(f"duplicate uid {uid} on {seen[uid]} and {code}")
        seen.setdefault(uid, code)

    if errors:
        print("ERROR: refusing to import:")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)

    changed = 0
    for code, uid in incoming.items():
        obj, label = by_code[code]
        old = (obj.get("nfc_uid") or "").upper()
        if old != uid:
            changed += 1
            print(f"  {code} {label}: {old or '(vacia)'} -> {uid or '(vacia)'}")
            obj["nfc_uid"] = uid

    assigned = sum(1 for code in by_code if (by_code[code][0].get("nfc_uid") or ""))
    missing = [code for code in by_code if not by_code[code][0].get("nfc_uid")]
    print(f"\n{changed} change(s). {assigned}/{len(by_code)} cards now have a UID.")
    if missing:
        print(f"Missing: {', '.join(missing)}")

    if args.dry_run:
        print("(dry run: nothing written)")
        return

    DATA_PATH.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    subprocess.run([sys.executable, str(GEN_SCRIPT)], check=True)
    print("\nNext: cd firmware && ./run_firmware.sh")


if __name__ == "__main__":
    main()
