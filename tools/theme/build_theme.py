#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_theme.py — offline builder of the project's OWN facsimile visual
style ("7explorer Aero"), a .msstyles in Windows 7 format.

Legal/technical notes:
- 100% original content authored by this project (colors, metrics, names).
  No bytes, bitmaps or property dumps from any Microsoft theme are used.
  Format knowledge comes from public documentation only: the msstyleEditor
  wiki (.msstyles Format page, MIT-licensed project) and the Windows SDK
  public headers (vsstyle.h / tmschema.h property + part id numbers).
- A .msstyles is a PE with resource section only: CMAP (class names,
  UTF-16), VARIANT/NORMAL (32-byte header + typed payload records,
  8-byte aligned), IMAGE/STREAM atlases, PACKTHEM_VERSION, RT_STRING.
- Property records: see load_msstyles_spec() below. Scalar records are
  40 bytes (32 header + 4 data + 4 pad); string/margins carry variable
  data padded to 8; FILENAME/FONT pack the referenced id in 'shortFlag'.

v0 scope: structural probe themes (colors only, no atlases) used by
diag-theme CI to learn how uxtheme reacts to an unsigned, self-authored
theme before the full facsimile content lands.
"""
import argparse
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "tests"))
import pebuilder  # noqa: E402  (tests/pebuilder.py, local deterministic builder)

# ---- msstyles property format ----------------------------------------------
# typeID values (from public docs / SDK TMT): see IDENTIFIER in the MIT
# msstyleEditor enum (re-derived from the .msstyles Format wiki page).
T_STRING = 201
T_INT = 202
T_BOOL = 203
T_COLOR = 204
T_MARGINS = 205
T_FILENAME = 206

# nameID constants we use (public Windows SDK vsstyle.h vs. historical
# tmschema.h; numeric values are a functional interface, not content):
TMT_DISPLAYNAME = 601
TMT_COMPANY = 603
TMT_AUTHOR = 604
TMT_COPYRIGHT = 605
TMT_VERSION = 607
TMT_SIZINGMARGINS = 3601
TMT_CONTENTMARGINS = 3602
TMT_FILLCOLOR = 3802
TMT_TEXTCOLOR = 3803
TMT_HEIGHT = 2417
TMT_FLATMENUS = 1001

# SDK part ids (tmschema.h, public)
TBP_BACKGROUNDBOTTOM = 1
TBP_BACKGROUNDRIGHT = 2
TBP_BACKGROUNDTOP = 3
TBP_BACKGROUNDLEFT = 4

# Class indices = position in CMAP. Order is OUR choice (indices are
# opaque); names must match what the shell asks uxtheme for.
CLASSES = ["GLOBALS", "TASKBAR", "TASKBAND", "REBAR", "STARTPANEL",
           "TRAYNOTIFY", "CLOCK", "MENU", "MENUBAND"]
CID_GLOBALS, CID_TASKBAR = 0, 1


def rgb(r, g, b):
    """COLORREF 0x00BBGGRR"""
    return (b << 16) | (g << 8) | r


# Our own authored Aero-like palette (original values, not lifted):
PALETTE = {
    "taskbar_fill": rgb(0x35, 0x62, 0x8C),   # deep aero blue
    "taskbar_fill_hi": rgb(0x6C, 0xA3, 0xD8),
    "text_on_taskbar": rgb(0xFF, 0xFF, 0xFF),
    "globals_accent": rgb(0x10, 0x3C, 0x68),
}


class Rec:
    def __init__(self, name, type_, cid, part, state, value):
        self.name, self.type = name, type_
        self.cid, self.part, self.state = cid, part, state
        self.value = value

    def bytes(self):
        """32-byte header + payload, padded so rec len % 8 == 0."""
        if self.type in (T_INT, T_BOOL, T_COLOR):
            data = struct.pack("<i", self.value) + b"\0" * 4
            size = 4
        elif self.type == T_COLOR:
            raise AssertionError("unreachable")
        elif self.type == T_MARGINS:
            l, t, r, b = self.value
            data = struct.pack("<iiii", l, t, r, b)
            size = len(data)
            data += b"\0" * ((-len(data)) % 4)
            # records are aligned to 8 total (32+n*8): margins 16B -> 48
        elif self.type == T_STRING:
            raw = (self.value + "\0").encode("utf-16-le")
            size = len(raw)
            data = raw
            data += b"\0" * ((-(32 + len(data))) % 8)
        elif self.type == T_FILENAME:
            # value packed in shortFlag, no payload
            rec = struct.pack("<8i", self.name, self.type, self.cid,
                              self.part, self.state, int(self.value), 0, 0)
            return rec
        else:
            raise ValueError(f"type {self.type} unsupported")
        # scalar: 32+4+4 = 40 (mult of 8) OK; margins: 32+16=48 OK;
        # string already aligned.  For int/color/bool we fixed 8 bytes data.
        if self.type in (T_INT, T_BOOL, T_COLOR):
            assert (32 + len(data)) % 8 == 0
        else:
            assert (32 + len(data)) % 8 == 0, len(data)
        return struct.pack("<8i", self.name, self.type, self.cid,
                           self.part, self.state, 0, 0, size) + data


def build_variant(records):
    """Sort by (classID, partID, stateID, nameID) and serialize."""
    records = sorted(records, key=lambda r: (r.cid, r.part, r.state, r.name))
    return b"".join(r.bytes() for r in records)


def build_cmap(classes):
    return "".join(c + "\0\0" for c in classes).encode("utf-16-le")


def make_records_probe():
    recs = []
    g = CID_GLOBALS
    recs.append(Rec(TMT_DISPLAYNAME, T_STRING, g, 0, 0, "7explorer Aero"))
    recs.append(Rec(TMT_COMPANY, T_STRING, g, 0, 0, "7explorer project"))
    recs.append(Rec(TMT_AUTHOR, T_STRING, g, 0, 0, "7explorer project"))
    recs.append(Rec(TMT_COPYRIGHT, T_STRING, g, 0, 0,
                    "Original work of the 7explorer project"))
    recs.append(Rec(TMT_VERSION, T_STRING, g, 0, 0, "1.0"))
    tb = CID_TASKBAR
    for part in (TBP_BACKGROUNDBOTTOM, TBP_BACKGROUNDRIGHT,
                 TBP_BACKGROUNDTOP, TBP_BACKGROUNDLEFT):
        recs.append(Rec(TMT_FILLCOLOR, T_COLOR, tb, part, 0,
                        PALETTE["taskbar_fill"]))
        recs.append(Rec(TMT_SIZINGMARGINS, T_MARGINS, tb, part, 0,
                        (4, 4, 4, 4)))
    return recs


def build_theme(sig128: bytes | None):
    """Returns (pe_bytes, stats). sig128: None = no signature trailer,
    otherwise appended with the community-documented footer structure
    (magic 0x84692426, sigSize, fileSize, 0) — still NOT a valid
    cryptographic signature; used only to probe uxtheme's behaviour."""
    cmap = build_cmap(CLASSES)
    variant = build_variant(make_records_probe())
    packthem = struct.pack("<HH", 4, 0)  # v4 documented for Vista+
    resources = [
        ("CMAP", "CMAP", 0x0409, cmap),
        ("VARIANT", "NORMAL", 0x0409, variant),
        ("PACKTHEM_VERSION", "PACKTHEM_VERSION", 0x0409, packthem),
    ]
    pe, _ = pebuilder.build_resource_pe(resources)
    stats = {"cmap": len(cmap), "variant": len(variant), "classes":
             len(CLASSES), "signed": sig128 is not None}
    if sig128 is not None:
        blob = pe
        footer = struct.pack("<IIII", 0x84692426, len(sig128),
                             len(blob) + len(sig128) + 16, 0)
        blob = blob + sig128 + footer
        return blob, stats
    return pe, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--sig", choices=["none", "dummy"], default="none",
                    help="append a dummy 128-byte signature trailer")
    args = ap.parse_args()
    sig = None
    if args.sig == "dummy":
        # 128 bytes, OUR OWN filler pattern (documented as non-crypto)
        sig = bytes((0x7E, 0x58) * 64)
    blob, stats = build_theme(sig)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(blob)
    print(f"build_theme: wrote {args.out} ({len(blob)} bytes) {stats}")


if __name__ == "__main__":
    main()
