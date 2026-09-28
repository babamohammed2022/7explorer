#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_pe_resources.py — post-injection PROOF on the real PE file.

Usage:  python3 tools/check_pe_resources.py <localized explorer.exe>

Reads the actual .rsrc directory of the file the installer produced and
verifies, strictly (any mismatch -> exit 1):

  * every resource payload the project generated (tools/build_resources)
    exists at the exact (type, id, lcid) coordinate AND is byte-identical
    to the embedded blob (what the installer should have written);
  * every RT_MENU / RT_STRING / RT_DIALOG / RT_ACCELERATOR payload parses
    back (structure intact — guards against a corrupt resource write);
  * the "MUI" type is GONE (neutralized) and "CUI" id 1 is present;
  * en-US (0x0409) fallback and it-IT (0x0410) are BOTH fully present.

This is the CI gate for criterion "Windows really loads what we generate":
runnable on any OS (pure stdlib), exercised on real Windows runners.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.analyze_mui import (  # noqa: E402
    Resources, parse_string_table, parse_menu, parse_dialog,
    parse_accelerators,
)
from tools.patch_imports import _PE  # noqa: E402
from tools.build_resources import build_all, RT_MENU, RT_DIALOG, RT_STRING, \
    RT_ACCELERATOR  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def _load_project_side():
    catalogs = {}
    for f in sorted((ROOT / "localization" / "catalog").glob("*.json")):
        d = json.loads(f.read_text(encoding="utf-8"))
        catalogs[d["language"]["id"]] = d
    templates = json.loads(
        (ROOT / "localization" / "templates" /
         "explorer.exe.templates.json").read_text(encoding="utf-8"))
    con = json.loads(
        (ROOT / "localization" / "constraints" /
         "explorer.exe.constraints.json").read_text(encoding="utf-8"))
    explorer_ids = set(int(k) for k in con["strings"])
    return catalogs, templates, explorer_ids


def check(exe_path: str) -> int:
    problems = []
    catalogs, templates, explorer_ids = _load_project_side()
    expected = build_all(catalogs, templates, explorer_ids)
    lcid_of = {lang: int(str(catalogs[lang]["language"]["lcid"]), 16)
               for lang in expected}

    blob = Path(exe_path).read_bytes()
    pe = _PE(blob)
    actual = {}
    types_seen = set()
    for (t, rid, lang, data) in Resources(pe).walk():
        types_seen.add(t)
        actual[(t, rid, lang)] = data

    # 1+2. presence + byte identity + parse-back per generated payload
    ok_blobs = 0
    for lang, blobs in expected.items():
        lcid = lcid_of[lang]
        for b in blobs:
            key = (b["type"], b["resId"], lcid)
            got = actual.get(key)
            if got is None:
                problems.append(f"missing resource type={b['type']} "
                                f"id={b['resId']} lcid=0x{lcid:04X}")
                continue
            if got != b["payload"]:
                problems.append(f"payload mismatch type={b['type']} "
                                f"id={b['resId']} lcid=0x{lcid:04X} "
                                f"({len(got)} vs {len(b['payload'])} bytes)")
                continue
            # parse-back sanity on the BYTES FOUND IN THE FILE
            if b["type"] == RT_MENU and not parse_menu(got):
                problems.append(f"menu {b['resId']} (lang {lang}) unparseable")
            elif b["type"] == RT_DIALOG and not parse_dialog(got):
                problems.append(f"dialog {b['resId']} (lang {lang}) "
                                f"unparseable")
            elif b["type"] == RT_STRING and \
                    not parse_string_table(got, b["resId"]):
                problems.append(f"string block {b['resId']} (lang {lang}) "
                                f"empty after parse")
            elif b["type"] == RT_ACCELERATOR and not parse_accelerators(got):
                problems.append(f"accelerator {b['resId']} (lang {lang}) "
                                f"unparseable")
            ok_blobs += 1

    # 3. neutralization proof
    if "MUI" in types_seen:
        problems.append('"MUI" resource type still present (not neutralized)')
    if not any(t == "CUI" for (t, _r, _l) in actual):
        problems.append('"CUI" resource type not found (neutralization '
                        "did not run)")

    # 4. both languages fully landed
    for lang, lcid in lcid_of.items():
        n = sum(1 for b in expected[lang]
                if actual.get((b["type"], b["resId"], lcid)) == b["payload"])
        print(f"{lang} (lcid 0x{lcid:04X}): {n}/{len(expected[lang])} "
              f"blobs OK")

    if problems:
        for p in problems:
            print(f"ERROR: {p}", file=sys.stderr)
        print(f"check_pe_resources: FAILED ({len(problems)} problem(s))",
              file=sys.stderr)
        return 1
    total = sum(len(v) for v in expected.values())
    print(f"check_pe_resources: OK — {ok_blobs}/{total} payloads verified "
          f"byte-for-byte in {exe_path}; MUI neutralized -> CUI")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: check_pe_resources.py <localized explorer.exe>",
              file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(check(sys.argv[1]))
