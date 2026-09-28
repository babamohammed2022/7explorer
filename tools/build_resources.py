#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_resources.py — deterministic PE-resource PAYLOAD generation.

Inputs (all in-repo, zero Microsoft text/binaries):
  * localization/templates/explorer.exe.templates.json
      FULL structural descriptor (styles, rects, class atoms, cmds — NO
      text; may be regenerated from the reference file with
      `analyze_mui.py --templates` and diffed with --compare below)
  * localization/catalog/<lang>.json
      our own texts (en.json fallback + it.json)

Output:
  list of {type, res_id, payload} per language, fed to
  tools/embed_catalog.py which writes installer/ex7selfcontained/
  lang_catalog.h byte blobs. Runtime: the installer injects these blobs
  into the private explorer.exe copy — no external .mui ever read.

Every generated payload is round-trip validated by parsing it back with
tools/analyze_mui.py and diffing structure + texts. Any mismatch raises
BuildError -> embed aborts -> CI fails BEFORE shipping a broken shell.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.analyze_mui import (  # noqa: E402
    parse_string_table, parse_menu, parse_dialog, parse_accelerators,
)

RT_MENU, RT_DIALOG, RT_STRING, RT_ACCELERATOR = 4, 5, 6, 9

EXPLORER_LANG_ID = 0x0409  # risorse di riferimento: lang 0409


class BuildError(Exception):
    pass


# ---------------------------------------------------------------- builders

def _wstrz(s: str) -> bytes:
    return s.encode("utf-16-le") + b"\0\0"


def _pad4(b: bytearray) -> None:
    while len(b) & 3:
        b.append(0)


def _sz_or_ord(kind) -> bytes:
    """kind: {"kind":"null"|"atom"|"string", "value":...}"""
    if kind is None or kind.get("kind") == "null":
        return b"\0\0"
    if kind.get("kind") == "atom":
        return struct.pack("<HH", 0xFFFF, kind.get("value", 0))
    if kind.get("kind") == "string":
        return _wstrz(kind.get("value", ""))
    raise BuildError(f"sz_or_ord: bad kind {kind}")


def build_string_block(block_id: int, texts: dict) -> bytes:
    """texts: {string_id(int within block range): text} — missing = empty."""
    base = (block_id - 1) * 16
    out = bytearray()
    for i in range(16):
        s = texts.get(base + 1 + i, "")
        out += struct.pack("<H", len(s))
        out += s.encode("utf-16-le")
    return bytes(out)


def build_menu(template: dict, texts: dict) -> bytes:
    """texts: {"level/index": text}. Structure from template descriptor."""
    version = template["version"]
    levels = template["levels"]
    if version == 0:
        out = bytearray(struct.pack("<HH", 0, 0))

        def emit(level_items, li):
            pops = []
            for ii, it in enumerate(level_items):
                flags = it["flags"]
                text = texts.get(f"{li}/{ii}")
                out.extend(struct.pack("<H", flags))
                if text is not None:
                    out.extend(_wstrz(text))
                else:
                    out.extend(b"\0\0")
                if it["popup"]:
                    pops.append(it["popup_level"])
                else:
                    out.extend(struct.pack("<H", it["cmd"] & 0xFFFF))
            for sub in pops:  # children stored AFTER all level siblings
                emit(levels[sub], sub)

        emit(levels[0], 0)
        return bytes(out)
    if version == 1:  # RT_MENU "version 4" / MENUEX (wVersion=1)
        out = bytearray(struct.pack("<HHI", 1, 4, 0))

        def emit_ex(level_items, li):
            pops = []
            for ii, it in enumerate(level_items):
                text = texts.get(f"{li}/{ii}")
                out.extend(struct.pack("<IIIB", it["dwType"], it["dwState"],
                                       it["cmd"] & 0xFFFFFFFF,
                                       it["bResInfo"] & 0xFF))
                out.extend(_wstrz(text) if text is not None else b"\0\0")
                _pad4(out)
                if it["popup"]:
                    out.extend(struct.pack("<I", it.get("help_id", 0)))
                    pops.append(it["popup_level"])
            for sub in pops:  # children stored AFTER all level siblings
                emit_ex(levels[sub], sub)

        emit_ex(levels[0], 0)
        return bytes(out)
    raise BuildError(f"menu version {version} not supported")


def build_dialog(template: dict, title: str | None, ctl_texts: dict) -> bytes:
    """DIALOGEX32 payload (our reference dialogs are all ex)."""
    if not template.get("ex"):
        raise BuildError("only DIALOGEX supported (reference is all-ex)")
    out = bytearray()
    out += struct.pack("<HH", 0xFFFF, 0xFFFF)
    out += struct.pack("<I", template.get("help_id") or 0)
    out += struct.pack("<I", template.get("ex_style", 0))
    out += struct.pack("<I", template["style"])
    ctls = template["controls"]
    out += struct.pack("<H", len(ctls))
    out += struct.pack("<hhhh", *template["rect"])
    out += _sz_or_ord(template.get("menu"))
    out += _sz_or_ord(template.get("window_class"))
    out += _wstrz(title or "")
    if template["style"] & 0x40:  # DS_SETFONT / DS_SHELLFONT
        font = template.get("font") or {}
        out += struct.pack("<HHBB", font.get("points", 8),
                           font.get("weight") or 400,
                           font.get("italic") or 0,
                           font.get("charset") or 0)
        face = font.get("face") or {"kind": "string",
                                    "value": "MS Shell Dlg"}
        out += _sz_or_ord(face if isinstance(face, dict)
                          else {"kind": "string", "value": face})
    seen = {}
    for c in ctls:
        _pad4(out)
        out += struct.pack("<I", c.get("help_id") or 0)
        out += struct.pack("<I", c.get("ex_style", 0))
        out += struct.pack("<I", c.get("style", 0x50000000))
        out += struct.pack("<hhhh", *c["rect"])
        out += struct.pack("<I", c["id"] & 0xFFFFFFFF)
        cls = c["class"]
        if cls.get("kind") == "atom":
            atom = cls.get("value")
            if atom is None:  # {"kind":"atom","name":"BUTTON"}
                name2atom = {"BUTTON": 0x0080, "EDIT": 0x0081,
                             "STATIC": 0x0082, "LISTBOX": 0x0083,
                             "SCROLLBAR": 0x0084, "COMBOBOX": 0x0085}
                atom = name2atom.get(cls.get("name", ""), 0x0082)
            out += struct.pack("<HH", 0xFFFF, atom)
        else:
            out += _wstrz(cls.get("name", ""))
        cid = c["id"] & 0xFFFFFFFF
        text = None
        if c.get("text"):
            seen[cid] = seen.get(cid, 0) + 1
            occ = seen[cid]
            dup = sum(1 for x in ctls if (x["id"] & 0xFFFFFFFF) == cid
                      and x.get("text"))
            key = f"{cid}#{occ}" if dup > 1 else str(cid)
            text = ctl_texts.get(key)
        out += _wstrz(text if text is not None else "")
        extra = bytes.fromhex(c.get("extra_hex", "") or "")
        out += struct.pack("<H", len(extra))
        out += extra
    return bytes(out)


def build_accelerators(rows: list) -> bytes:
    out = bytearray()
    n = len(rows)
    for i, r in enumerate(rows):
        fv = r["fVirt"] & 0xFF
        if i == n - 1:
            fv |= 0x80
        out += struct.pack("<BBHHH", fv, 0, r["key"], r["cmd"], 0)
    return bytes(out)


# ------------------------------------------------------------ validation

def validate_menu(payload: bytes, template: dict) -> None:
    m = parse_menu(payload)
    if not m or not m.get("levels"):
        raise BuildError("menu payload does not round-trip parse")
    lev_t = template["levels"]
    lev_p = m["levels"]
    if len(lev_t) != len(lev_p):
        raise BuildError(f"menu level count {len(lev_p)} != {len(lev_t)}")
    for li, (it_t, it_p) in enumerate(zip(lev_t, lev_p)):
        if len(it_t) != len(it_p):
            raise BuildError(f"menu level {li}: {len(it_p)} items != "
                             f"{len(it_t)}")
        for ii, (t_, p_) in enumerate(zip(it_t, it_p)):
            if bool(t_.get("popup")) != bool(p_.get("popup")):
                raise BuildError(f"menu {li}/{ii}: popup mismatch")
            if not t_.get("popup") and (t_.get("cmd") & 0xFFFF) != \
                    (p_.get("cmd") & 0xFFFF):
                raise BuildError(f"menu {li}/{ii}: cmd mismatch")


def validate_dialog(payload: bytes, template: dict,
                    expected_texts: dict) -> None:
    d = parse_dialog(payload)
    if not d:
        raise BuildError("dialog payload does not round-trip parse")
    if d["rect"] != template["rect"]:
        raise BuildError("dialog rect mismatch")
    if len(d["controls"]) != len(template["controls"]):
        raise BuildError("dialog control count mismatch")
    for got, want in zip(d["controls"], template["controls"]):
        if got["rect"] != want["rect"] or got["id"] != want["id"]:
            raise BuildError("dialog control geometry mismatch")


# ------------------------------------------------------------------ driver

def build_all(catalogs: dict, templates: dict,
              explorer_string_ids: set) -> dict:
    """Returns {lang: [ {type,resId,payload}, ... ]} for languages that
    cover the explorer surface (strings + menus + dialogs)."""
    langs = [k for k in ("en", "it") if k in catalogs]
    out = {}
    for lang in langs:
        data = catalogs[lang]
        strings = {int(k): v for k, v in data["strings"].items()}
        if any(sid not in strings for sid in explorer_string_ids):
            continue  # no explorer blocks for partial languages
        payloads = []
        # STRING blocks touched by explorer ids
        blocks = sorted({(sid - 1) // 16 + 1 for sid in explorer_string_ids})
        for bid in blocks:
            texts = {sid: strings[sid] for sid in explorer_string_ids
                     if (sid - 1) // 16 + 1 == bid}
            payload = build_string_block(bid, texts)
            back = parse_string_table(payload, bid)
            for sid, want in texts.items():
                if back.get(sid) != want:
                    raise BuildError(f"string {sid} round-trip mismatch")
            payloads.append({"type": RT_STRING, "resId": bid,
                             "payload": payload})
        # MENUs
        for mkey, mtemplate in templates["menus"].items():
            rid = int(mkey.split("/")[0])
            texts = data.get("menus", {}).get(mkey, {})
            payload = build_menu(mtemplate, texts)
            validate_menu(payload, mtemplate)
            payloads.append({"type": RT_MENU, "resId": rid,
                             "payload": payload})
        # DIALOGs
        for dkey, dtemplate in templates["dialogs"].items():
            rid = int(dkey.split("/")[0])
            dentry = data.get("dialogs", {}).get(dkey, {})
            payload = build_dialog(dtemplate, dentry.get("title"),
                                   dentry.get("controls", {}))
            validate_dialog(payload, dtemplate, dentry)
            payloads.append({"type": RT_DIALOG, "resId": rid,
                             "payload": payload})
        # ACCELERATORs
        for akey, rows in templates.get("accelerators", {}).items():
            rid = int(akey.split("/")[0])
            payload = build_accelerators(rows)
            back = parse_accelerators(payload)
            if [r["key"] for r in back] != [r["key"] for r in rows] or \
               [r["cmd"] for r in back] != [r["cmd"] for r in rows]:
                raise BuildError("accelerator round-trip mismatch")
            payloads.append({"type": RT_ACCELERATOR, "resId": rid,
                             "payload": payload})
        out[lang] = payloads
    return out


def compare_templates(ours_path, theirs_path) -> int:
    """--compare: structural diff between our descriptor and a reference
    dump (analyze_mui --templates). Prints diffs, returns #problems."""
    ours = json.loads(Path(ours_path).read_text(encoding="utf-8"))
    theirs = json.loads(Path(theirs_path).read_text(encoding="utf-8"))
    n = 0

    def cmplists(a, b, where, fields):
        nonlocal n
        if len(a) != len(b):
            print(f"{where}: count {len(a)} != {len(b)}")
            n += 1
            return
        for i, (x, y) in enumerate(zip(a, b)):
            for f in fields:
                if x.get(f) != y.get(f):
                    print(f"{where}[{i}].{f}: {x.get(f)} != {y.get(f)}")
                    n += 1

    for key in sorted(set(ours["menus"]) | set(theirs["menus"])):
        a, b = ours["menus"].get(key), theirs["menus"].get(key)
        if not a or not b or a.get("version") != b.get("version"):
            print(f"menu {key}: presence/version mismatch"); n += 1; continue
        if len(a["levels"]) != len(b["levels"]):
            print(f"menu {key}: level count mismatch"); n += 1; continue
        for li, (la, lb) in enumerate(zip(a["levels"], b["levels"])):
            cmplists(la, lb, f"menu {key} L{li}",
                     ["flags", "cmd", "dwType", "dwState", "bResInfo",
                      "popup", "popup_level"])
    for key in sorted(set(ours["dialogs"]) | set(theirs["dialogs"])):
        a, b = ours["dialogs"].get(key), theirs["dialogs"].get(key)
        if not a or not b:
            print(f"dialog {key}: presence mismatch"); n += 1; continue
        for f in ["style", "ex_style", "help_id", "rect", "ex"]:
            if a.get(f) != b.get(f):
                print(f"dialog {key}.{f}: {a.get(f)} != {b.get(f)}")
                n += 1
        cmplists(a["controls"], b["controls"], f"dialog {key}.controls",
                 ["id", "rect", "style", "ex_style", "help_id"])
    print(f"compare_templates: {n} difference(s)")
    return n


def _demo_selfcheck() -> int:
    root = Path(__file__).resolve().parent.parent
    cat = {}
    for f in (root / "localization" / "catalog").glob("*.json"):
        d = json.loads(f.read_text(encoding="utf-8"))
        cat[d["language"]["id"]] = d
    tpl = json.loads((root / "localization" / "templates" /
                      "explorer.exe.templates.json").read_text(
        encoding="utf-8"))
    con = json.loads((root / "localization" / "constraints" /
                      "explorer.exe.constraints.json").read_text(
        encoding="utf-8"))
    res = build_all(cat, tpl, set(int(k) for k in con["strings"]))
    total = sum(len(v) for v in res.values())
    for lang, blobs in res.items():
        print(f"{lang}: {len(blobs)} resource payloads")
    print(f"build_resources: OK ({total} payloads)")
    return 0


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--compare", nargs=2, metavar=("OURS", "REFERENCE"),
                    help="diff vs analyze_mui --templates output")
    args = ap.parse_args()
    if args.compare:
        raise SystemExit(1 if compare_templates(*args.compare) else 0)
    raise SystemExit(_demo_selfcheck())
