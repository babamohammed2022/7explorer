#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_catalog.py — automatic structural verification of the embedded
language catalog against the reference constraints.

    python3 tools/verify_catalog.py \
        --catalog-dir localization/catalog \
        --constraints localization/constraints/shell32.dll.constraints.json \
        [--constraints localization/constraints/explorer.exe.constraints.json]

Checks (every failure exits 1, CI-ready):

  * the fallback language (en) exists and covers EVERY constrained string;
  * every catalog string ID exists in the constraints (no orphan text);
  * placeholders match EXACTLY (number, order and type, %1!s!/%s/%d/...);
  * at most ONE accelerator '&' per string;
  * accelerator uniqueness inside every "coexisting" context
    (same menu / same dialog);
  * length budget: translated length <= max(2x reference, reference+8)
    unless the string opts out with a per-language "maxlen_override"
    entry (must be accompanied by a "comment" explaining why).

This is the script required by the task ("script di verifica automatica
che controlli segnaposto e acceleratori di ogni lingua rispetto alla
struttura originale e fallisca in caso di differenze"). Reference TEXT is
never loaded or compared here: only structure.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.analyze_mui import (  # noqa: E402
    extract_placeholders, extract_accel, norm_token, strip_accels,
)

FALLBACK_LANG = "en"


def load_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def merge_constraints(paths):
    merged = {"strings": {}, "contexts": {}, "menus": {}, "dialogs": {},
              "sources": []}
    for p in paths:
        c = load_json(Path(p))
        merged["sources"].append(c.get("source", str(p)))
        merged["strings"].update(c.get("strings", {}))
        merged["contexts"].update(c.get("contexts", {}))
        merged["menus"].update(c.get("menus", {}))
        merged["dialogs"].update(c.get("dialogs", {}))
    return merged


def verify(catalog_dir: Path, constraints_paths) -> int:
    errors = []
    warnings = []
    con = merge_constraints(constraints_paths)
    cstrings = con["strings"]

    files = sorted(catalog_dir.glob("*.json"))
    if not files:
        errors.append(f"no language files in {catalog_dir}")
        return report(errors, warnings)

    catalogs = {}
    for f in files:
        try:
            data = load_json(f)
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{f.name}: unreadable: {exc}")
            continue
        lang = data.get("language", {}).get("id", f.stem)
        catalogs[lang] = data

    if FALLBACK_LANG not in catalogs:
        errors.append(f"missing fallback language '{FALLBACK_LANG}'")
        return report(errors, warnings)

    en = catalogs[FALLBACK_LANG]
    en_strings = en.get("strings", {})

    # --- fallback coverage ------------------------------------------------
    for sid in cstrings:
        if sid not in en_strings:
            errors.append(
                f"{FALLBACK_LANG}: missing string {sid} "
                f"(fallback must cover every constrained id)")

    # --- per language checks ----------------------------------------------
    for lang, data in sorted(catalogs.items()):
        strings = data.get("strings", {})

        # orphans: string ids that exist in no constraints file
        for sid in strings:
            if sid not in cstrings:
                errors.append(f"{lang}: orphan string id {sid} "
                              f"(no such id in the reference structure)")

        overrides = data.get("maxlen_override", {})

        for sid, text in strings.items():
            if sid not in cstrings:
                continue
            ref = cstrings[sid]
            where = f"{lang}:{sid}"
            if not isinstance(text, str):
                errors.append(f"{where}: not a string")
                continue

            # 1) placeholders: exact ordered match (number, order, type)
            try:
                got = [norm_token(t) for t in extract_placeholders(text)]
            except ValueError as exc:
                errors.append(f"{where}: {exc}")
                continue
            want = ref.get("placeholders", [])
            if got != want:
                errors.append(
                    f"{where}: placeholders {got} != reference {want}")

            # 2) accelerator: at most one '&'
            try:
                accel = extract_accel(text)
            except ValueError as exc:
                errors.append(f"{where}: {exc}")
                continue

            # 3) length budget
            budget = max(ref.get("len", 0) * 2, ref.get("len", 0) + 8)
            ov = overrides.get(sid)
            if ov:
                comment = (data.get("maxlen_override_comment", {})
                           .get(sid, ""))
                if isinstance(ov, int):
                    budget = ov
                if not comment:
                    errors.append(
                        f"{where}: maxlen_override without a comment")
            plain_len = len(strip_accels(text))
            if plain_len > budget:
                errors.append(
                    f"{where}: length {plain_len} exceeds budget {budget} "
                    f"(reference len {ref.get('len')})")
            elif plain_len > ref.get("len", 0) * 1.5 and ref.get("len"):
                warnings.append(
                    f"{where}: length {plain_len} > 150% of reference "
                    f"({ref.get('len')}); check dialog/menu fit")

        # 4) accelerator uniqueness per context
        for ctx_name, ctx in con.get("contexts", {}).items():
            if not ctx.get("coex", True):
                continue
            seen = {}
            for sid in ctx.get("members", []):
                text = strings.get(sid)
                if not isinstance(text, str):
                    continue
                try:
                    a = extract_accel(text)
                except ValueError:
                    continue
                if a is None:
                    warnings.append(
                        f"{lang}:{ctx_name}: {sid} has no accelerator")
                else:
                    key = a.lower()
                    if key in seen:
                        errors.append(
                            f"{lang}:{ctx_name}: accelerator '&{a}' used by "
                            f"both {seen[key]} and {sid}")
                    seen[key] = sid

    return report(errors, warnings)


def report(errors, warnings) -> int:
    for w in warnings:
        print(f"WARNING: {w}")
    for e in errors:
        print(f"ERROR: {e}")
    print(f"verify_catalog: {len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalog-dir", required=True)
    ap.add_argument("--constraints", action="append", required=True,
                    help="constraints JSON (repeatable)")
    args = ap.parse_args()
    return verify(Path(args.catalog_dir), args.constraints)


if __name__ == "__main__":
    raise SystemExit(main())
