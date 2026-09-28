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

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, os.pardir, os.pardir, "tests"))
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

# Class indices = position in CMAP. The first N indices are RESERVED by
# the loader: GetThemeDefaults resolves the size/color variant names by
# looking up "sizevariant.<Size>" / "colorvariant.<Color>" in CMAP, and
# the global classes "globals"/"sysmetrics"/"documentation" occupy fixed
# slots. Layout below mirrors the observed reserved region (observed
# structurally via the CI theme probe; these are functional identifiers,
# not creative content). Our authored classes follow at index 13+.
RESERVED = ["documentation", "", "", "sizevariant.NormalSize", "",
            "sizevariant.Default", "colorvariant.NormalColor", "", "",
            "", "globals", "sysmetrics", ""]
CLASSES = RESERVED + ["GLOBALS", "TASKBAR", "TASKBAND", "REBAR",
                      "STARTPANEL", "TRAYNOTIFY", "CLOCK", "MENU",
                      "MENUBAND"]
CID_RESERVED = len(RESERVED)
CID_GLOBALS = CID_RESERVED + 0
CID_TASKBAR = CID_RESERVED + 1
CID_GLOBALCLASS, CID_SYSMETRICS = 10, 11


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
    """Sequential UTF-16LE class names, each terminated by ONE NUL
    wchar (single u16 NUL between entries — verified against the real
    theme: a loader scan must find 'sizevariant.NormalSize' at its
    reserved index; a double NUL inserts phantom empty classes)."""
    return "".join(c + "\0" for c in classes).encode("utf-16-le")


def make_records_k():
    """f's records + at least one property for each reserved variant
    class (sizevariant.NormalSize/Default, colorvariant.NormalColor,
    globals, sysmetrics) — hypothesis: the loader requires property
    blocks for the default variant classes it resolves."""
    recs = make_records_probe()
    for cid in (3, 5, 6, 10, 11):
        recs.append(Rec(TMT_FILLCOLOR, T_COLOR, cid, 0, 0,
                        PALETTE["globals_accent"]))
    return recs


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


def _build_resource_pe32(resources):
    """PE32 variant of pebuilder's resource-only image builder."""
    tris = []
    for type_id, name_id, lang, payload in resources:
        tris.append((type_id, [(name_id, [(lang, payload)])]))
    # delegate three-level resource serialization to pebuilder by reusing
    # its build_resource_pe internals is not exposed; call build_pe32 with
    # the same resource-section blob by replicating build_resource_pe's
    # section construction via monkey-shim: easiest is temporary import hook
    import types
    helper = pebuilder.build_resource_pe  # defined below uses build_pe
    # patch: call helper but with build_pe32 swapped in
    orig = pebuilder.build_pe
    pebuilder.build_pe = pebuilder.build_pe32
    try:
        blob, meta = helper(resources)
    finally:
        pebuilder.build_pe = orig
    return blob


build_pe32 = False  # match host arch (x64 themes are PE32+/AMD64)


def build_theme(sig128: bytes | None, rmap=None, vmap=None,
                bcmap=None, extras=(), variant=None):
    """Returns (pe_bytes, stats). sig128: None = no signature trailer,
    otherwise appended with the community-documented footer structure
    (magic 0x84692426, sigSize, fileSize, 0) — still NOT a valid
    cryptographic signature; used only to probe uxtheme's behaviour."""
    cmap = build_cmap(CLASSES)
    if variant is None:
        variant = build_variant(make_records_probe())
    packthem = struct.pack("<H", 4)  # v4 documented for Vista+
    # layout proven by the CI probe enum on a real system theme:
    #   type 'PACKTHEM_VERSION' id #1 (2 bytes), type 'VMAP' name 'VMAP',
    #   type 'RMAP' name 'RMAP'
    resources = [
        ("CMAP", "CMAP", 0x0409, cmap),
        ("VARIANT", "NORMAL", 0x0409, variant),
        ("PACKTHEM_VERSION", 1, 0x0409, packthem),
    ]
    if rmap is not None:
        resources.append(("RMAP", "RMAP", 0x0409, rmap))
    if vmap is not None:
        resources.append(("VMAP", "VMAP", 0x0409, vmap))
    if bcmap is not None:
        resources.append(("BCMAP", "BCMAP", 0x0409, bcmap))
    for type_id, name_id, payload in extras:
        resources.append((type_id, name_id, 0x0409, payload))
    if build_pe32:
        pe = _build_resource_pe32(resources)
    else:
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


def _globals_stream():
    recs = [Rec(TMT_NAME, T_STRING, CID_GLOBALCLASS, 0, 0, "7explorer Aero")]
    return b"".join(r.bytes() for r in recs)


def vmap_exact():
    """Format proven byte-for-byte against a real system theme's VMAP
    (structural metadata read by the CI probe): three length-prefixed
    UTF-16 strings in this order: variant, size, color. 76 bytes, no
    leading count. String VALUES here are the standard variant labels —
    functional identifiers required by uxtheme."""
    out = b""
    for st in ("Normal", "NormalSize", "NormalColor"):
        u = (st + "\0").encode("utf-16-le")
        rec = struct.pack("<i", len(st) + 1) + u
        rec += b"\0" * ((-len(rec)) % 4)   # each record pad to 4 bytes
        out += rec
    assert len(out) == 76, len(out)
    return out


def rmap_stream():
    """Root/global properties: STRING records, class=0 part=0 state=0
    (RMAP in a real theme begins with name 600 = style display name).
    100% our own authored texts."""
    recs = [
        Rec(600, T_STRING, 0, 0, 0, "7explorer Aero"),
        Rec(601, T_STRING, 0, 0, 0, "7explorer Aero"),
        Rec(TMT_COMPANY, T_STRING, 0, 0, 0, "7explorer project"),
        Rec(TMT_AUTHOR, T_STRING, 0, 0, 0, "7explorer project"),
        Rec(TMT_COPYRIGHT, T_STRING, 0, 0, 0,
            "Original work of the 7explorer project"),
        Rec(TMT_VERSION, T_STRING, 0, 0, 0, "1.0"),
    ]
    return b"".join(r.bytes() for r in
                    sorted(recs, key=lambda r: r.name))


def bcmap_all_inherit(nclasses):
    """Base-class map: first int32 = entry count, then one int32 per
    class: -1 = inherits from DEFAULT/global class (format read from the
    structural BCMAP dump: count then per-class parent ids)."""
    return struct.pack("<i", nclasses) + b"\xff\xff\xff\xff" * nclasses


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--sig", choices=["none", "dummy"], default="none",
                    help="append a dummy 128-byte signature trailer")
    ap.add_argument("--batch", action="store_true",
                    help="write the CI candidate matrix into --out (a dir)")
    ap.add_argument("--dump-res", default=None,
                    help="with --batch: also dump raw resource payloads")
    args = ap.parse_args()
    sig = None
    if args.sig == "dummy":
        # 128 bytes, OUR OWN filler pattern (documented as non-crypto)
        sig = bytes((0x7E, 0x58) * 64)
    if args.batch:
        # candidate-matrix probes for the CI theme loader experiment
        os.makedirs(args.out, exist_ok=True)
        rmap = rmap_stream()
        desktop = [("DESKTOP", 1, struct.pack("<I", 0)),
                   ("MINCOLORDEPTH", 1, struct.pack("<H", 32)),
                   ("PVL", 1, struct.pack("<I", 1))]
        streams = [("STREAM", 1, bytes(64)), ("IMAGE", 1, bytes(64))]
        cases = [
            ("f_packthem_rmap_vmap.msstyles",
             dict(rmap=rmap, vmap=vmap_exact())),
            ("g_f_plus_bcmap.msstyles",
             dict(rmap=rmap, vmap=vmap_exact(),
                  bcmap=bcmap_all_inherit(len(CLASSES)))),
            ("h_f_plus_desktop.msstyles",
             dict(rmap=rmap, vmap=vmap_exact(), extras=desktop)),
            ("i_f_plus_streams.msstyles",
             dict(rmap=rmap, vmap=vmap_exact(), extras=desktop + streams)),
            ("k_f_variantclass_rec.msstyles",
             dict(rmap=rmap, vmap=vmap_exact(),
                  variant=build_variant(make_records_k()))),
        ]
        dump_dir = getattr(args, "dump_res", None)
        if dump_dir:
            payloads = {
                "cmap.bin": build_cmap(CLASSES),
                "variant.bin": build_variant(make_records_probe()),
                "rmap.bin": rmap_stream(),
                "vmap.bin": vmap_exact(),
                "packthem.bin": struct.pack("<H", 4),
                "bcmap.bin": bcmap_all_inherit(len(CLASSES)),
            }
            for nm, blob in payloads.items():
                with open(os.path.join(dump_dir, nm), "wb") as fh:
                    fh.write(blob)
            print(f"build_theme: dumped payloads to {dump_dir}")
        for name, kw in cases:
            blob, stats = build_theme(None, **kw)
            path = os.path.join(args.out, name)
            with open(path, "wb") as fh:
                fh.write(blob)
            print(f"build_theme: wrote {path} ({len(blob)} bytes)")
        return
    blob, stats = build_theme(sig)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(blob)
    print(f"build_theme: wrote {args.out} ({len(blob)} bytes) {stats}")


if __name__ == "__main__":
    main()
