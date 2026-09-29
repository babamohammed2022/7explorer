# structure_probe.py — structural probe of a PE's resources (NO text).
#
# Emits a JSON describing the STRUCTURE of menu/resources inside a given
# PE file. Same legal basis as the original BOZZE extraction: numeric
# fields, lengths, accelerators only — never the actual text bytes.
# CI runs it transiently on the symbol-server copy for REFERENCE ONLY;
# the output goes to the ci-logs branch (not into the repo artifacts).
#
# usage: structure_probe.py <pe-path> [--menus] [--strings] [--out <file>]
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_mui import Resources, parse_menu_full, parse_string_table  # noqa
from patch_imports import _PE  # noqa

RT_MENU = 4
RT_STRING = 6


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("--menus", action="store_true", default=True)
    ap.add_argument("--strings", action="store_true", default=True)
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    blob = Path(args.pe).read_bytes()
    pe = _PE(blob)
    entries = list(Resources(pe).walk())

    out = {"menus": {}, "strings": {}}

    if args.menus:
        for t, rid, lang, data in entries:
            if t == RT_MENU:
                desc = parse_menu_full(data)
                if desc:
                    out["menus"][f"{rid}/lang:{lang:04X}"] = desc

    if args.strings:
        for t, rid, lang, data in entries:
            if t == RT_STRING:
                table = parse_string_table(data, rid)
                # structure only: id -> length of the string (no chars)
                out["strings"][f"{rid}/lang:{lang:04X}"] = {
                    str(sid): len(txt) for sid, txt in table.items()
                }

    js = json.dumps(out, indent=2, sort_keys=True)
    if args.out == "-":
        print(js)
    else:
        Path(args.out).write_text(js, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
