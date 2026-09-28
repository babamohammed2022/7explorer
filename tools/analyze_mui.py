#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_mui.py — PE resource-structure analyzer for the self-contained
installer project.

Run it (Windows or Linux) on the REFERENCE binaries the user already owns
(e.g. the Win7 explorer.exe and its explorer.exe.mui):

    python3 tools/analyze_mui.py explorer.exe --dump-headers
    python3 tools/analyze_mui.py explorer.exe.mui --constraints out.json
    python3 tools/analyze_mui.py explorer.exe.mui --with-strings

It answers three questions needed by docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md:

  1. --dump-headers : TimeDateStamp / SizeOfImage / imported DLL names /
                      MUI resource presence -> verifies (or refutes) the
                      pinned constants in installer config.
  2. default/JSON   : full structural map of string tables, menus, dialogs
                      and accelerators (IDs, languages, geometry) WITHOUT
                      copyrighted text, unless --with-strings is given.
  3. --constraints  : minimal "constraints JSON" consumed by
                      tools/verify_catalog.py: per string ID the placeholder
                      list, max length and original accelerator letter; per
                      menu/dialog the structure our generator must reproduce.

NOTE: files produced by this tool are STRUCTURE ONLY (plus, with
--with-strings, reference text that must NOT be committed to the repo —
see README/licensing notes).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tools.patch_imports import _PE, PEFormatError  # noqa: E402

RT_CURSOR, RT_BITMAP, RT_ICON, RT_MENU, RT_DIALOG, RT_STRING, \
    RT_ACCELERATOR, RT_RCDATA, RT_MESSAGETABLE, RT_VERSION, RT_HTML, \
    RT_MANIFEST = (1, 2, 3, 4, 5, 6, 9, 10, 11, 16, 23, 24)

TYPE_NAMES = {
    RT_CURSOR: "CURSOR", RT_BITMAP: "BITMAP", RT_ICON: "ICON",
    RT_MENU: "MENU", RT_DIALOG: "DIALOG", RT_STRING: "STRINGTABLE",
    RT_ACCELERATOR: "ACCELERATORS", RT_RCDATA: "RCDATA",
    RT_MESSAGETABLE: "MESSAGETABLE", RT_VERSION: "VERSION",
    RT_HTML: "HTML", RT_MANIFEST: "MANIFEST",
}

# --------------------------------------------------------------------------
# placeholder / accelerator tokens
# --------------------------------------------------------------------------

_PLACEHOLDER_RE = re.compile(
    r"%\d+![a-zA-Z]+!"      # %1!s!   (FormatMessage style)
    r"|%I64[duxX]"          # %I64u
    r"|%l[csduxX]"          # %ls
    r"|%\d+"                # %1      (bare positional)
    r"|%[csduxX]"           # %s %d %u ...
    r"|%%")


def extract_placeholders(text: str):
    """Ordered list of placeholder tokens; raises on a bare '%'."""
    tokens = [m.group(0) for m in _PLACEHOLDER_RE.finditer(text)]
    stripped = _PLACEHOLDER_RE.sub("", text)
    if "%" in stripped:
        raise ValueError(f"unmatched '%' in: {text!r}")
    return tokens


def extract_accel(text: str):
    """Accelerator letter (char after '&'), or None; raises on two accels."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == "&":
            if i + 1 < len(text) and text[i + 1] == "&":
                i += 2
                continue
            if i + 1 < len(text):
                out.append(text[i + 1])
        i += 1
    if len(out) > 1:
        raise ValueError(f"multiple accelerators in: {text!r}")
    return out[0] if out else None


def strip_accels(text: str) -> str:
    return text.replace("&&", "\x00").replace("&", "").replace("\x00", "&")


def u16_cstr_end(raw: bytes, off: int) -> int:
    """Index just past the NUL of the UTF-16LE string starting at off.

    A byte-wise find(b"\\0\\0", off) can stop one byte early when it matches
    between a character's trailing zero and the terminator's first zero
    (e.g. 's' = 73 00 followed by 00 00 -> match at the odd offset).
    The terminator must land at the same parity as `off`.
    """
    end = raw.find(b"\0\0", off)
    if end == -1:
        raise ValueError("unterminated UTF-16 string in resource data")
    if (end - off) & 1:
        end += 1
    return end


def norm_token(tok: str) -> str:
    """Canonical form for comparison: %1!S! -> %1!s! ; %D -> %d ..."""
    if tok == "%%":
        return tok
    m = re.fullmatch(r"%(\d+)!([a-zA-Z]+)!", tok)
    if m:
        return f"%{m.group(1)}!{m.group(2).lower()}!"
    return tok.lower()


# --------------------------------------------------------------------------
# resource walking
# --------------------------------------------------------------------------

class Resources:
    def __init__(self, pe: _PE):
        self.pe = pe
        rva, _sz = pe.directory(2)  # IMAGE_DIRECTORY_ENTRY_RESOURCE
        self.res_rva = rva
        self.res_off = pe.rva_to_off(rva) if rva else None

    def _read_name(self, raw: int):
        if raw & 0x80000000:
            off = self.res_off + (raw & 0x7FFF)
            (ln,) = struct.unpack_from("<H", self.pe.data, off)
            return self.pe.data[off + 2: off + 2 + 2 * ln].decode(
                "utf-16-le", "replace")
        return raw

    def walk(self):
        """Yield (type, name, lang, data_bytes)."""
        if self.res_off is None:
            return
        root = self.res_off
        (_chars, _ts, _mv, _nv, n_named, n_id) = struct.unpack_from(
            "<IIHHHH", self.pe.data, root)
        for i in range(n_named + n_id):
            name_raw, ent_raw = struct.unpack_from(
                "<II", self.pe.data, root + 16 + 8 * i)
            t = self._read_name(name_raw)
            if not (ent_raw & 0x80000000):
                continue
            type_dir = self.res_off + (ent_raw & 0x7FFFFFFF)
            (_c, _t2, _m2, _n2, nn2, ni2) = struct.unpack_from(
                "<IIHHHH", self.pe.data, type_dir)
            for j in range(nn2 + ni2):
                n_raw, e_raw = struct.unpack_from(
                    "<II", self.pe.data, type_dir + 16 + 8 * j)
                rid = self._read_name(n_raw)
                if not (e_raw & 0x80000000):
                    continue
                lang_dir = self.res_off + (e_raw & 0x7FFFFFFF)
                (_c3, _t3, _m3, _n3, nn3, ni3) = struct.unpack_from(
                    "<IIHHHH", self.pe.data, lang_dir)
                for k in range(nn3 + ni3):
                    l_raw, de_raw = struct.unpack_from(
                        "<II", self.pe.data, lang_dir + 16 + 8 * k)
                    de_off = self.res_off + de_raw
                    data_rva, size, cp, _rsvd = struct.unpack_from(
                        "<IIII", self.pe.data, de_off)
                    d_off = self.pe.rva_to_off(data_rva)
                    yield (t, rid, l_raw,
                           self.pe.data[d_off: d_off + size])


# --------------------------------------------------------------------------
# parsers for the resource types we care about
# --------------------------------------------------------------------------

def parse_string_table(block: bytes, block_id: int):
    """Return {string_id: text} for a 16-string STRINGTABLE block."""
    out = {}
    off = 0
    base = (block_id - 1) * 16
    for i in range(16):
        if off + 2 > len(block):
            break
        (ln,) = struct.unpack_from("<H", block, off)
        off += 2
        raw = block[off: off + 2 * ln]
        off += 2 * ln
        if ln:
            out[base + i] = raw.decode("utf-16-le", "replace")
    return out


def parse_menu(raw: bytes):
    """Parse classic (v0) and extended (v4) menu templates.

    Returns {"version": n, "levels": [ [ {text, accel, cmd, popup, flags} ] ]}
    level 0 = top bar, popups get their own appended levels.
    """
    if len(raw) < 4:
        return None
    version, header = struct.unpack_from("<HH", raw, 0)
    levels = []

    def classic_full(off):
        """Win32 MENUITEMTEMPLATE: flags, [id unless MF_POPUP], text.
        A popup's children follow it immediately (depth-first)."""
        items = []
        lvl = len(levels)
        levels.append(items)
        while True:
            (flags,) = struct.unpack_from("<H", raw, off)
            off += 2
            is_popup = bool(flags & 0x10)
            cmd = None
            if not is_popup:
                (cmd,) = struct.unpack_from("<H", raw, off)
                off += 2
            end = u16_cstr_end(raw, off)
            text = raw[off:end].decode("utf-16-le", "replace")
            off = end + 2
            item = {"text": text, "accel": extract_accel(text),
                    "cmd": cmd, "popup": None, "flags": flags}
            items.append(item)
            if is_popup:
                item["popup"], off = classic_full(off)
            if flags & 0x80:
                break
        return lvl, off

    def extended(off):
        """MENUEX_TEMPLATE_ITEM: dwType, dwState, uId (DWORD), wFlags (WORD),
        text, DWORD align, dwHelpId only for popups; children depth-first."""
        items = []
        lvl = len(levels)
        levels.append(items)
        while True:
            dwType, dwState, uId, wFlags = struct.unpack_from("<IIIH",
                                                              raw, off)
            off += 14
            end = u16_cstr_end(raw, off)
            text = raw[off:end].decode("utf-16-le", "replace")
            off = end + 2
            off = (off + 3) & ~3
            is_popup = bool(wFlags & 0x01)
            item = {"text": text, "accel": extract_accel(text),
                    "cmd": None if is_popup else uId, "popup": None,
                    "type": dwType, "state": dwState}
            items.append(item)
            if is_popup:
                off += 4  # dwHelpId
                item["popup"], off = extended(off)
            if wFlags & 0x80:
                break
        return lvl, off

    if version == 0:
        classic_full(4)
        return {"version": 0, "levels": levels}
    if version == 1 and header == 4:
        extended(4 + header)
        return {"version": 1, "levels": levels}
    return {"version": version, "levels": None,
            "note": "unrecognized menu template"}


_KNOWN_CTRL_ATOMS = {0x0080: "BUTTON", 0x0081: "EDIT", 0x0082: "STATIC",
                     0x0083: "LISTBOX", 0x0084: "SCROLLBAR",
                     0x0085: "COMBOBOX"}


def parse_dialog(raw: bytes):
    """Parse DLGTEMPLATE / DLGTEMPLATEEX with their item lists."""
    if len(raw) < 4:
        return None
    sig, sig2 = struct.unpack_from("<HH", raw, 0)
    is_ex = (sig == 0xFFFF and sig2 == 0xFFFF)

    def sz_or_ord(off):
        (w,) = struct.unpack_from("<H", raw, off)
        if w == 0x0000:
            return None, off + 2
        if w == 0xFFFF:
            (o,) = struct.unpack_from("<H", raw, off + 2)
            return ("atom", o), off + 4
        end = u16_cstr_end(raw, off)
        return raw[off:end].decode("utf-16-le", "replace"), end + 2

    if is_ex:
        help_id, ex_style, style, c_items = struct.unpack_from("<IIIH", raw, 4)
        off = 4 + 14
    else:
        style, ex_style, c_items = struct.unpack_from("<IIH", raw, 0)
        off = 10
    x, y, cx, cy = struct.unpack_from("<hhhh", raw, off)
    off += 8
    menu, off = sz_or_ord(off)
    cls, off = sz_or_ord(off)
    title, off = sz_or_ord(off)
    font = None
    if style & 0x40:  # DS_SETFONT
        if is_ex:
            pts, weight, italic, charset = struct.unpack_from("<HHBB", raw, off)
            off += 6
        else:
            (pts,) = struct.unpack_from("<H", raw, off)
            off += 2
        face, off = sz_or_ord(off)
        font = {"points": pts, "face": face}
    controls = []
    item_struct = 18 if is_ex else 14
    for _ in range(c_items):
        off = (off + 3) & ~3
        if is_ex:
            _h, _ex, _st = struct.unpack_from("<III", raw, off)
            ix, iy, icx, icy = struct.unpack_from("<hhhh", raw, off + 12)
            (cid,) = struct.unpack_from("<I", raw, off + 20)
            off += 24
        else:
            _st, _ex = struct.unpack_from("<II", raw, off)
            ix, iy, icx, icy = struct.unpack_from("<hhhh", raw, off + 8)
            (cid,) = struct.unpack_from("<H", raw, off + 16)
            off += 18
        c_cls, off = sz_or_ord(off)
        c_txt, off = sz_or_ord(off)
        (extra,) = struct.unpack_from("<H", raw, off)
        off += 2 + extra
        if isinstance(c_cls, tuple) and c_cls[0] == "atom":
            c_cls = _KNOWN_CTRL_ATOMS.get(c_cls[1], f"atom:{c_cls[1]}")
        controls.append({"id": cid, "class": c_cls, "rect": [ix, iy, icx, icy],
                         "text": c_txt,
                         "accel": extract_accel(c_txt) if c_txt else None})
    return {"ex": is_ex, "rect": [x, y, cx, cy], "title": title,
            "title_accel": extract_accel(title) if title else None,
            "font": font, "style": style, "controls": controls}




def build_templates(path: str) -> dict:
    """FULL structural descriptor for tools/build_resources.py.

    Like build_constraints but keeps EVERY structural field needed to
    rebuild the resource templates byte-compatible (styles, exstyles,
    help ids, font block, class ordinals/strings, creation data).
    Reference TEXT is replaced by stable keys (same keying as the
    catalogs): never emits UI prose. Font and window-class names are
    technical identifiers, documented as such.
    """
    blob = open(path, "rb").read()
    pe = _PE(blob)
    out = {
        "source": ("explorer.exe.mui full structural template dump "
                   "(analyze_mui --templates; NO UI text)"),
        "source_sha256": hashlib.sha256(blob).hexdigest(),
        "source_file_size": len(blob),
        "time_date_stamp": f"0x{pe.time_date_stamp:08X}",
        "machine": f"0x{pe.machine:04X}",
        "menus": {},
        "dialogs": {},
        "accelerators": {},
        "notes": ("Font face names and window class names (SysLink, "
                  "SysTreeView32, ...) are public Win32 API identifiers, "
                  "not prose text. Any extra data blob that LOOKS textual "
                  "is flagged and must be reviewed before use."),
    }
    for t, rid, lang, data in Resources(pe).walk():
        key = f"{rid}/lang:{lang:04X}"
        try:
            if t == RT_MENU:
                m = parse_menu_full(data)
                if m: out["menus"][key] = m
            elif t == RT_DIALOG:
                d = parse_dialog_full(data)
                if d: out["dialogs"][key] = d
            elif t == RT_ACCELERATOR:
                out["accelerators"][key] = parse_accelerators(data)
        except Exception as exc:
            out.setdefault("parse_errors", []).append(
                {"type": str(t), "id": str(rid), "lang": lang,
                 "error": str(exc)})
    return out


def parse_menu_full(raw: bytes):
    """Full menu descriptor: every numeric field, texts as len/accel only.

    Level/index numbers match parse_menu walking exactly (menu 205/6003
    classic v0, altri MENUEX v4).
    """
    if len(raw) < 4:
        return None
    version, header = struct.unpack_from("<HH", raw, 0)
    items_by_level = {}
    level_counter = [0]

    def classic_store(off, lvl):
        items_by_level.setdefault(lvl, [])
        items = items_by_level[lvl]
        while True:
            (flags,) = struct.unpack_from("<H", raw, off)
            off += 2
            entry = {"flags": flags, "popup": bool(flags & 0x10)}
            if entry["popup"]:
                entry["cmd"] = None
            else:
                (cmd,) = struct.unpack_from("<H", raw, off)
                off += 2
                entry["cmd"] = cmd
            end = u16_cstr_end(raw, off)
            text = raw[off:end].decode("utf-16-le", "replace")
            off = end + 2
            if text:
                entry["text"] = {"len": len(text),
                                 "accel": extract_accel(text)}
            items.append(entry)
            if entry["popup"]:
                child = level_counter[0]
                level_counter[0] += 1
                entry["popup_level"] = child
                off = classic_store(off, child)
            if flags & 0x80:
                break
        return off

    def extended_store(off, lvl):
        items_by_level.setdefault(lvl, [])
        items = items_by_level[lvl]
        while True:
            dwType, dwState, uId, wFlags = struct.unpack_from("<IIIH",
                                                              raw, off)
            off += 14
            end = u16_cstr_end(raw, off)
            text = raw[off:end].decode("utf-16-le", "replace")
            off = end + 2
            off = (off + 3) & ~3
            entry = {"dwType": dwType, "dwState": dwState, "cmd": uId,
                     "bResInfo": wFlags,
                     "popup": bool(wFlags & 0x01)}
            if text:
                entry["text"] = {"len": len(text),
                                 "accel": extract_accel(text)}
            items.append(entry)
            if entry["popup"]:
                (help_id,) = struct.unpack_from("<I", raw, off)
                off += 4
                entry["help_id"] = help_id
                child = level_counter[0]
                level_counter[0] += 1
                entry["popup_level"] = child
                off = extended_store(off, child)
            if wFlags & 0x80:
                break
        return off

    if version == 0:
        level_counter[0] = 1
        classic_store(4, 0)
        return {"version": 0,
                "levels": [items_by_level.get(i, [])
                           for i in range(level_counter[0])]}
    if version == 1 and header == 4:
        level_counter[0] = 1
        extended_store(8, 0)
        return {"version": 4,
                "levels": [items_by_level.get(i, [])
                           for i in range(level_counter[0])]}
    return {"version": version, "levels": None,
            "note": "unrecognized menu template"}


def parse_dialog_full(raw: bytes):
    """Full dialog descriptor: every structural field (title/control text
    replaced by len+accel markers; class atoms AND class strings kept)."""
    if len(raw) < 4:
        return None
    sig, sig2 = struct.unpack_from("<HH", raw, 0)
    is_ex = (sig == 0xFFFF and sig2 == 0xFFFF)

    def sz_or_ord2(off):
        (w,) = struct.unpack_from("<H", raw, off)
        if w == 0x0000:
            return {"kind": "null"}, off + 2
        if w == 0xFFFF:
            (o,) = struct.unpack_from("<H", raw, off + 2)
            return {"kind": "atom", "value": o}, off + 4
        end = u16_cstr_end(raw, off)
        s = raw[off:end].decode("utf-16-le", "replace")
        return {"kind": "string", "value": s}, end + 2

    if is_ex:
        help_id, ex_style, style, c_items = struct.unpack_from("<IIIH",
                                                               raw, 4)
        off = 18
    else:
        help_id = None
        style, ex_style, c_items = struct.unpack_from("<IIH", raw, 0)
        off = 10
    x, y, cx_, cy = struct.unpack_from("<hhhh", raw, off)
    off += 8
    menu, off = sz_or_ord2(off)
    cls, off = sz_or_ord2(off)
    title, off = sz_or_ord2(off)
    title_meta = None
    if title.get("kind") == "string":
        title_meta = {"len": len(title["value"]),
                      "accel": extract_accel(title["value"])}
        title = {"kind": "text"}  # testo proibito, solo presenza
    font = None
    if style & 0x40:
        if is_ex:
            pts, weight, italic, charset = struct.unpack_from("<HHBB",
                                                              raw, off)
            off += 6
        else:
            (pts,) = struct.unpack_from("<H", raw, off)
            weight = italic = charset = None
            off += 2
        face, off = sz_or_ord2(off)
        font = {"points": pts, "weight": weight, "italic": italic,
                "charset": charset,
                "face": face.get("value") if face.get("kind") == "string"
                else face}
    controls = []
    for _ in range(c_items):
        off = (off + 3) & ~3
        if is_ex:
            c_help, c_exs, c_st = struct.unpack_from("<III", raw, off)
            ix, iy, icx, icy = struct.unpack_from("<hhhh", raw, off + 12)
            (cid,) = struct.unpack_from("<I", raw, off + 20)
            off += 24
        else:
            c_help = None
            c_st, c_exs = struct.unpack_from("<II", raw, off)
            ix, iy, icx, icy = struct.unpack_from("<hhhh", raw, off + 8)
            (cid,) = struct.unpack_from("<H", raw, off + 16)
            off += 18
        c_cls, off = sz_or_ord2(off)
        if c_cls.get("kind") == "atom":
            c_cls = {"kind": "atom",
                     "name": _KNOWN_CTRL_ATOMS.get(c_cls["value"],
                                                   f'atom:{c_cls["value"]}')}
        c_txt, off = sz_or_ord2(off)
        tmeta = None
        if c_txt.get("kind") == "string":
            tmeta = {"len": len(c_txt["value"]),
                     "accel": extract_accel(c_txt["value"])}
        (extra,) = struct.unpack_from("<H", raw, off)
        off += 2
        extra_hex = raw[off:off + extra].hex()
        extra_textual = _looks_utf16_text(raw[off:off + extra]) if extra else False
        off += extra
        controls.append({
            "id": cid, "help_id": c_help, "ex_style": c_exs, "style": c_st,
            "rect": [ix, iy, icx, icy], "class": c_cls,
            "text": tmeta,
            "extra_hex": extra_hex,
            "extra_looks_textual": extra_textual})
    return {"ex": is_ex, "help_id": help_id, "ex_style": ex_style,
            "style": style, "rect": [x, y, cx_, cy], "menu": menu,
            "window_class": cls, "title": title_meta, "font": font,
            "controls": controls}

def _looks_utf16_text(raw: bytes) -> bool:
    """True se la maggior parte dei word sono stampabili (euristica)."""
    if len(raw) < 4 or len(raw) % 2:
        return False
    words = struct.unpack(f"<{len(raw)//2}H", raw)
    printable = sum(1 for w in words if 0x20 <= w <= 0x7E or w >= 0xA0)
    return printable * 4 >= len(words) * 3

def parse_accelerators(raw: bytes):
    out = []
    for off in range(0, len(raw) - 7, 8):
        fv, key, cmd, _pad = struct.unpack_from("<HHHH", raw, off)
        out.append({"fVirt": fv, "key": key, "cmd": cmd,
                    "last": bool(fv & 0x80)})
    return out


# --------------------------------------------------------------------------
# report assembly
# --------------------------------------------------------------------------

def build_report(path: str, with_strings: bool) -> dict:
    blob = open(path, "rb").read()
    pe = _PE(blob)
    rep = {
        "file": path,
        "sha256": hashlib.sha256(blob).hexdigest(),
        "size": len(blob),
        "machine": f"0x{pe.machine:04X}",
        "time_date_stamp": f"0x{pe.time_date_stamp:08X}",
        "size_of_image": f"0x{pe.size_of_image:X}",
        "imports": [],
        "has_mui_resource": False,
        "named_rcdata": [],
        "strings": {},   # id -> {"lang": lcid, "len": n, "accel": c, "placeholders": [...]}
        "menus": {},
        "dialogs": {},
        "accelerators": {},
        "other_types": {},
    }
    for _off, name_rva, _t in pe.import_descriptors():
        rep["imports"].append(
            pe.read_cstr(pe.rva_to_off(name_rva)).decode("ascii", "replace"))

    for t, rid, lang, data in Resources(pe).walk():
        tname = TYPE_NAMES.get(t, t) if isinstance(t, int) else f'"{t}"'
        if t == RT_RCDATA and rid == "MUI":
            rep["has_mui_resource"] = True
        if t == RT_RCDATA and isinstance(rid, str):
            rep["named_rcdata"].append(rid)
        key = f"{rid}/lang:{lang:04X}"
        try:
            if t == RT_STRING and isinstance(rid, int):
                strings = parse_string_table(data, rid)
                for sid, text in strings.items():
                    entry = {"lang": f"0x{lang:04X}", "len": len(text),
                             "accel": extract_accel(text),
                             "placeholders": [norm_token(tk) for tk in
                                              extract_placeholders(text)]}
                    if with_strings:
                        entry["text"] = text
                    rep["strings"][str(sid)] = entry
            elif t == RT_MENU:
                m = parse_menu(data)
                if m and not with_strings:
                    for lvl in (m["levels"] or []):
                        for it in lvl:
                            it.pop("text", None)
                rep["menus"][key] = m
            elif t == RT_DIALOG:
                d = parse_dialog(data)
                if d and not with_strings:
                    d["title"] = None if d["title"] is None else "<text>"
                    for c in d["controls"]:
                        if c["text"] is not None:
                            c["text"] = "<text>"
                    if d["font"]:
                        d["font"].pop("face", None)
                rep["dialogs"][key] = d
            elif t == RT_ACCELERATOR:
                rep["accelerators"][key] = parse_accelerators(data)
            else:
                cnt = rep["other_types"].setdefault(str(tname), {"count": 0,
                                                                 "ids": []})
                cnt["count"] += 1
                cnt["ids"].append(rid if isinstance(rid, int) else f'"{rid}"')
        except Exception as exc:  # robustness: report, keep walking
            rep.setdefault("parse_errors", []).append(
                {"type": str(tname), "id": str(rid), "lang": lang,
                 "error": str(exc)})
    return rep


def build_constraints(rep: dict) -> dict:
    """Structure-only constraints for tools/verify_catalog.py."""
    con = {
        "source": rep["file"],
        "source_sha256": rep["sha256"],
        "strings": {},
        "menus": {},
        "dialogs": {},
    }
    for sid, e in rep["strings"].items():
        con["strings"][sid] = {
            "len": e["len"],
            "accel": e["accel"],
            "placeholders": e["placeholders"],
        }
    for key, m in (rep.get("menus") or {}).items():
        if not m or m.get("levels") is None:
            continue
        con["menus"][key] = {
            "levels": [[{"cmd": it["cmd"], "accel": it["accel"],
                         "popup": it["popup"],
                         "flags": it.get("flags")} for it in lvl]
                       for lvl in m["levels"]]}
    for key, d in (rep.get("dialogs") or {}).items():
        if not d:
            continue
        con["dialogs"][key] = {
            "title_len": d["rect"], "title_accel": d["title_accel"],
            "rect": d["rect"], "has_title": d["title"] is not None,
            "controls": [{"id": c["id"], "class": c["class"],
                          "rect": c["rect"], "has_text": c["text"] is not None,
                          "accel": c["accel"]} for c in d["controls"]]}
    return con


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pe_file")
    ap.add_argument("--with-strings", action="store_true",
                    help="include full reference text (DO NOT COMMIT)")
    ap.add_argument("--constraints", metavar="OUT.json",
                    help="write verify_catalog constraints JSON")
    ap.add_argument("--dump-headers", action="store_true",
                    help="print header/import info to verify pinned constants")
    ap.add_argument("--templates", metavar="OUT.json",
                    help="write FULL structural descriptor (build_resources)")
    args = ap.parse_args()

    try:
        rep = build_report(args.pe_file, args.with_strings)
    except (PEFormatError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.dump_headers:
        print(f"file           : {rep['file']}")
        print(f"sha256         : {rep['sha256']}")
        print(f"size           : {rep['size']}")
        print(f"machine        : {rep['machine']} (expect 0x8664)")
        print(f"TimeDateStamp  : {rep['time_date_stamp']}")
        print(f"SizeOfImage    : {rep['size_of_image']}")
        print(f"MUI resource   : {rep['has_mui_resource']}")
        if rep["named_rcdata"]:
            print(f"named RCDATA   : {sorted(set(rep['named_rcdata']))}")
        print("imports:")
        for n in rep["imports"]:
            marker = "  <-- patched to wrp64.dll" if n.upper() in (
                "SHLWAPI.DLL", "OLE32.DLL", "EXPLORERFRAME.DLL") else ""
            print(f"  {n}{marker}")

    if args.templates:
        tpl = build_templates(args.pe_file)
        with open(args.templates, "w", encoding="utf-8") as f:
            json.dump(tpl, f, ensure_ascii=False, indent=2)
        print(f"templates written to {args.templates}")
    if args.constraints:
        with open(args.constraints, "w", encoding="utf-8") as f:
            json.dump(build_constraints(rep), f, ensure_ascii=False, indent=2)
        print(f"constraints written to {args.constraints}")
    elif not args.dump_headers and not args.templates and not args.constraints:
        json.dump(rep, sys.stdout, ensure_ascii=False, indent=2)
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
