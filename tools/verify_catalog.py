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

  * the fallback language (en) exists and covers EVERY constrained string
    of every constraints file marked  "require_fallback_coverage": true;
  * every catalog string ID exists in the constraints (no orphan text);
  * placeholders match EXACTLY (number, order and type, %1!s!/%s/%d/...);
  * at most ONE accelerator '&' per string;
  * accelerator uniqueness inside every "coexisting" context
    (same menu level / same dialog);
  * length budget: translated length <= max(2x reference, reference+8)
    unless the string opts out with a per-language "maxlen_override"
    entry (must be accompanied by a comment explaining why);
  * optional dialogs/menus catalog sections are verified the same way:
    keys "<id>/lang:0409" -> {"title": ..., "controls": {...}} and
    "<id>/lang:0409" -> {"level/item": "..."}; duplicate control ids
    take "#n" suffixes in template order.

This is the script required by the task ("script di verifica automatica
che controlli segnaposto e acceleratori di ogni lingua rispetto alla
struttura originale e fallisca in caso di differenze"). Reference TEXT is
never loaded or compared here: only structure.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
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
              "sources": [],
              # [(name, strings_dict, require_en_coverage)]
              "coverage_sets": []}
    for p in paths:
        c = load_json(Path(p))
        merged["sources"].append(c.get("source", str(p)))
        merged["strings"].update(c.get("strings", {}))
        merged["contexts"].update(c.get("contexts", {}))
        merged["menus"].update(c.get("menus", {}))
        merged["dialogs"].update(c.get("dialogs", {}))
        merged["coverage_sets"].append(
            (c.get("source", str(p)), c.get("strings", {}),
             bool(c.get("require_fallback_coverage", False))))
    return merged


def budget_for(ref_len: int) -> int:
    return max(ref_len * 2, ref_len + 8)


def check_one(lang, where, text, ref_len, ref_placeholders,
              overrides, comments, override_key,
              errors, warnings):
    """Validate ONE catalog text against {len, placeholders} reference.

    Returns the accelerator letter (or None); appends problems."""
    if not isinstance(text, str):
        errors.append(f"{where}: not a string")
        return None

    try:
        got = [norm_token(t) for t in extract_placeholders(text)]
    except ValueError as exc:
        errors.append(f"{where}: {exc}")
        return None
    want = ref_placeholders or []
    if got != want:
        errors.append(f"{where}: placeholders {got} != reference {want}")

    try:
        accel = extract_accel(text)
    except ValueError as exc:
        errors.append(f"{where}: {exc}")
        return None

    budget = budget_for(ref_len)
    ov = overrides.get(override_key)
    if ov:
        comment = comments.get(override_key, "")
        if isinstance(ov, int):
            budget = ov
        if not comment:
            errors.append(f"{where}: maxlen_override without a comment")
    plain_len = len(strip_accels(text))
    if plain_len > budget:
        errors.append(f"{where}: length {plain_len} exceeds budget {budget} "
                      f"(reference len {ref_len})")
    elif ref_len and plain_len > ref_len * 1.5:
        warnings.append(
            f"{where}: length {plain_len} > 150% of reference "
            f"({ref_len}); check dialog/menu fit")
    return accel


def check_accel_uniqueness(pairs, where, lang, errors, warnings):
    """pairs = [(key, accel_or_None, ref_has_accel)] sharing one visual
    context. Missing accel is a WARNING only when the reference structure
    marks that element as mnemonic-bearing."""
    seen = {}
    for key, accel, ref_has in pairs:
        if accel is None:
            if ref_has:
                warnings.append(
                    f"{lang}:{where}: {key} has no accelerator "
                    f"(reference has one)")
            continue
        if not ref_has:
            warnings.append(
                f"{lang}:{where}: {key} has accelerator '&{accel}' "
                f"but reference has none")
        low = accel.lower()
        if low in seen:
            errors.append(
                f"{lang}:{where}: accelerator '&{accel}' used by both "
                f"{seen[low]} and {key}")
        seen[low] = key


def expected_dialog_controls(ref):
    """Map catalog key -> reference control for a dialog constraints entry.

    Catalog rule: controls carrying text in the reference are keyed by
    control id; when the same id appears more than once with text, keys
    become "<id>#N" (N from 1) in template order.
    """
    counts = Counter(c["id"] for c in ref.get("controls", []) if c.get("len"))
    seen = Counter()
    expected = {}
    for c in ref.get("controls", []):
        if not c.get("len"):
            continue
        cid = c["id"]
        seen[cid] += 1
        key = f"{cid}#{seen[cid]}" if counts[cid] > 1 else str(cid)
        expected[key] = c
    return expected


def expected_menu_items(ref):
    """Map "level/item" catalog key -> reference item (text items only)."""
    expected = {}
    for li, level in enumerate(ref.get("levels", [])):
        for ii, item in enumerate(level):
            if item.get("len"):
                expected[f"{li}/{ii}"] = (li, item)
    return expected


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

    # --- fallback coverage (only for sources that opt in) ----------------
    for source, strings, require in con["coverage_sets"]:
        if not require:
            continue
        for sid in strings:
            if sid not in en_strings:
                errors.append(
                    f"{FALLBACK_LANG}: missing string {sid} "
                    f"(fallback must cover every constrained id of "
                    f"'{source}')")

    # --- per language checks ----------------------------------------------
    for lang, data in sorted(catalogs.items()):
        strings = data.get("strings", {})
        overrides = data.get("maxlen_override", {})
        comments = data.get("maxlen_override_comment", {})

        # orphans: string ids that exist in no constraints file
        for sid in strings:
            if sid not in cstrings:
                errors.append(f"{lang}: orphan string id {sid} "
                              f"(no such id in the reference structure)")

        for sid, text in strings.items():
            if sid not in cstrings:
                continue
            ref = cstrings[sid]
            check_one(lang, f"{lang}:{sid}", text,
                      ref.get("len", 0), ref.get("placeholders", []),
                      overrides, comments, sid, errors, warnings)

        # 4) accelerator uniqueness per named string context
        for ctx_name, ctx in con.get("contexts", {}).items():
            if not ctx.get("coex", True):
                continue
            pairs = []
            for sid in ctx.get("members", []):
                text = strings.get(sid)
                if not isinstance(text, str):
                    continue
                try:
                    a = extract_accel(text)
                except ValueError:
                    continue
                ref_has = bool(cstrings.get(sid, {}).get("accel"))
                pairs.append((sid, a, ref_has))
            check_accel_uniqueness(pairs, ctx_name, lang, errors, warnings)

        # 5) dialogs section (optional)
        for dkey, dentry in data.get("dialogs", {}).items():
            ref = con["dialogs"].get(dkey)
            where_base = f"{lang}:{dkey}"
            if ref is None:
                errors.append(f"{where_base}: orphan dialog "
                              f"(no such dialog in the reference structure)")
                continue
            if not isinstance(dentry, dict):
                errors.append(f"{where_base}: dialog entry must be an "
                              f"object {{\"title\":..., \"controls\":...}}")
                continue

            pairs = []
            title = dentry.get("title")
            if title is not None:
                check_one(lang, f"{where_base}:title", title,
                          ref.get("title_len", 0), [],
                          overrides, comments, f"dialog:{dkey}:title",
                          errors, warnings)

            expected = expected_dialog_controls(ref)
            dctl = dentry.get("controls", {})
            if not isinstance(dctl, dict):
                errors.append(f"{where_base}: \"controls\" must be an object")
                continue
            for key, text in dctl.items():
                rc = expected.get(key)
                if rc is None:
                    errors.append(f"{where_base}: orphan control '{key}'")
                    continue
                accel = check_one(lang, f"{where_base}:{key}", text,
                                  rc.get("len", 0),
                                  rc.get("placeholders", []),
                                  overrides, comments,
                                  f"dialog:{dkey}:{key}",
                                  errors, warnings)
                if isinstance(text, str):
                    pairs.append((key, accel, bool(rc.get("accel"))))
            for key in expected:
                if key not in dctl:
                    warnings.append(f"{where_base}: missing text for "
                                    f"control {key}")
            check_accel_uniqueness(pairs, dkey, lang, errors, warnings)

        # 6) menus section (optional)
        for mkey, mentry in data.get("menus", {}).items():
            ref = con["menus"].get(mkey)
            where_base = f"{lang}:{mkey}"
            if ref is None:
                errors.append(f"{where_base}: orphan menu "
                              f"(no such menu in the reference structure)")
                continue
            if not isinstance(mentry, dict):
                errors.append(f"{where_base}: menu entry must be an object "
                              f"mapping \"level/item\" to text")
                continue
            expected = expected_menu_items(ref)
            levels = {}
            for key, text in mentry.items():
                got = expected.get(key)
                if got is None:
                    errors.append(f"{where_base}: orphan menu item '{key}'")
                    continue
                li, rc = got
                accel = check_one(lang, f"{where_base}:{key}", text,
                                  rc.get("len", 0),
                                  rc.get("placeholders", []),
                                  overrides, comments,
                                  f"menu:{mkey}:{key}",
                                  errors, warnings)
                if isinstance(text, str):
                    levels.setdefault(li, []).append(
                        (key, accel, bool(rc.get("accel"))))
            for key in expected:
                if key not in mentry:
                    warnings.append(f"{where_base}: missing text for "
                                    f"menu item {key}")
            for li, pairs in levels.items():
                check_accel_uniqueness(pairs, f"{mkey} level {li}",
                                       lang, errors, warnings)

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
