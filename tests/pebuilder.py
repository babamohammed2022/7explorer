# -*- coding: utf-8 -*-
"""
pebuilder.py — deterministic builders of tiny synthetic PE32+ (AMD64) images
used ONLY by the test-suite. Nothing here is shipped at runtime; the binaries
produced are minimal fixtures, not functional programs.
"""

from __future__ import annotations

import struct

FILE_ALIGN = 0x200
SECTION_ALIGN = 0x1000
IMAGE_BASE = 0x140000000


def _align(v: int, a: int) -> int:
    return (v + a - 1) & ~(a - 1)


def _dos_and_pe_stub() -> bytearray:
    buf = bytearray(0x80)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x80)
    return buf


def build_pe(sections, directories, characteristics=0x2022,
             time_date_stamp=0x4CE7A144):
    """
    sections: list of dicts {name, data(bytes), vsize, chars}
    directories: dict index -> (rva, size) to write into the 16-entry table;
                 entries named by absolute index (1=IMPORT, 2=RESOURCE,
                 11=BOUND IMPORT).
    Returns bytes of a well-formed (though non-executable) PE32+ image.
    """
    num_sections = len(sections)
    headers = _dos_and_pe_stub()
    coff_size = 20
    opt_size = 240  # PE32+ optional header incl. 16 data directories
    size_headers = _align(len(headers) + 4 + coff_size + opt_size + 40 * num_sections,
                          FILE_ALIGN)

    # layout
    cur_raw = size_headers
    cur_rva = SECTION_ALIGN
    for s in sections:
        s["raw_off"] = cur_raw
        s["raw_size"] = _align(len(s["data"]), FILE_ALIGN) if s["data"] else 0
        s["vaddr"] = cur_rva
        span = max(s.get("vsize", len(s["data"])), s["raw_size"])
        cur_raw += s["raw_size"]
        cur_rva += _align(span, SECTION_ALIGN)
    size_image = cur_rva

    total = size_headers + sum(s["raw_size"] for s in sections)
    out = bytearray(total)

    # DOS + PE signature + COFF
    out[0:len(headers)] = headers
    pe = 0x80
    out[pe:pe + 4] = b"PE\0\0"
    coff = pe + 4
    struct.pack_into("<HHIIIHH", out, coff,
                     0x8664, num_sections, time_date_stamp, 0, 0, opt_size,
                     characteristics)
    # Optional header (PE32+)
    opt = coff + 20
    struct.pack_into("<HBBIII", out, opt, 0x20B, 14, 0, 0x200, 0, 0)
    struct.pack_into("<II", out, opt + 16, SECTION_ALIGN, 0)  # EntryPoint, BaseOfCode
    struct.pack_into("<Q", out, opt + 24, IMAGE_BASE)
    struct.pack_into("<II", out, opt + 32, SECTION_ALIGN, FILE_ALIGN)
    struct.pack_into("<HHHHHH", out, opt + 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into("<I", out, opt + 52, 0)
    struct.pack_into("<II", out, opt + 56, size_image, size_headers)
    struct.pack_into("<I", out, opt + 64, 0)  # CheckSum
    struct.pack_into("<HH", out, opt + 68, 3, 0x8160)  # Subsystem, DllChars
    struct.pack_into("<QQQQ", out, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", out, opt + 104, 0, 16)  # LoaderFlags, NumberOfRvaAndSizes
    dirs_off = opt + 112
    for idx, (rva, size) in directories.items():
        struct.pack_into("<II", out, dirs_off + 8 * idx, rva, size)

    # Section headers + bodies
    sec_hdr = opt + opt_size
    for i, s in enumerate(sections):
        name = s["name"].encode("ascii")[:8].ljust(8, b"\0")
        struct.pack_into("<8sIIIIIIHHI", out, sec_hdr + 40 * i,
                         name, s.get("vsize", len(s["data"])), s["vaddr"],
                         s["raw_size"], s["raw_off"], 0, 0, 0, 0, s["chars"])
        out[s["raw_off"]:s["raw_off"] + len(s["data"])] = s["data"]

    return bytes(out), {s["name"]: s for s in sections}


def build_import_pe(dll_names, bound_import_names=None, mixed_case=None):
    """
    Build a PE importing each name in dll_names (with a couple of thunks),
    optionally with a (fake but well-shaped) bound import directory for
    bound_import_names. Returns (bytes, info).
    """
    blob = bytearray()
    names = []
    # 1) contiguous descriptor array (N descriptors + 1 all-zero terminator)
    for i, n in enumerate(dll_names):
        desc_off = len(blob)
        blob += b"\0" * 20  # fields patched below
        names.append((desc_off, n))
    blob += b"\0" * 20  # terminator
    # 2) name strings, then (unused) thunk storage
    name_offs = {}
    for desc_off, n in names:
        name_offs[desc_off] = len(blob)
        blob += n.encode("ascii") + b"\0"
        if len(blob) & 1:
            blob += b"\0"
    thunks_off = len(blob)
    blob += b"\0" * (16 * 2 * len(dll_names))  # ILT+IAT, 2 entries each

    # name RVAs are relative to section start; section .idata is the 2nd
    # section, so its virtual address is SECTION_ALIGN*2.
    base_rva = SECTION_ALIGN * 2
    for desc_off, n in names:
        struct.pack_into("<IIIII", blob, desc_off,
                         0, 0, 0, base_rva + name_offs[desc_off], 0)
    idata = bytes(blob)

    sections = [
        {"name": ".text", "data": b"\xC3" * 16, "chars": 0x60000020},
        {"name": ".idata", "data": idata, "chars": 0xC0000040},
    ]
    directories = {1: (SECTION_ALIGN * 2, len(idata))}

    if bound_import_names:
        bnd = bytearray()
        for n in bound_import_names:
            bnd += struct.pack("<IIHH", 0, 0, 0, 0)  # patched below
            bnd += n.encode("ascii") + b"\0"
            if len(bnd) & 1:
                bnd += b"\0"
        bnd += b"\0" * 8
        sections.append({"name": ".bind", "data": bytes(bnd), "chars": 0x40000040})
        directories[11] = (SECTION_ALIGN * 3, len(bnd))

    pe, secinfo = build_pe(sections, directories)
    return pe, secinfo


# ---------------------------------------------------------------------------
# Resource-only PE builder (for analyze_mui.py tests)
# ---------------------------------------------------------------------------

def _res_string_block(strings16):
    """strings16: list of exactly 16 python strings ('' for empty)."""
    assert len(strings16) == 16
    out = bytearray()
    for s in strings16:
        w = s.encode("utf-16-le")
        out += struct.pack("<H", len(s)) + w
    return bytes(out)


def _res_menu_classic(items, header=True):
    """items: list of (options:int, text:str, cmd_id:int|None).

    cmd_id None => popup entry. Top-level end marked by 0x80 in options.
    header=False emits the item list only, for embedding as a popup body.
    """
    out = bytearray(struct.pack("<HH", 0, 0)) if header else bytearray()
    for i, (opt, text, cid) in enumerate(items):
        last = (i == len(items) - 1)
        out += struct.pack("<H", opt | (0x80 if last else 0))
        out += text.encode("utf-16-le") + b"\0\0"
        if cid is not None:
            out += struct.pack("<H", cid)
    return bytes(out)


def _se_word_or_ord(v):
    if v is None:
        return struct.pack("<H", 0)
    if isinstance(v, int):
        return struct.pack("<HH", 0xFFFF, v)
    return v.encode("utf-16-le") + b"\0\0"


def _res_dialogex(caption, controls, pointsize=9, typeface="MS Shell Dlg",
                  style=0x84C800C4 | 0x00000080 | 0x00020000):
    """Minimal DIALOGEX template (DS_SETFONT|DS_SHELLFONT|WS_VISIBLE...)."""
    style |= 0x00000040  # DS_SETFONT
    out = bytearray()
    out += struct.pack("<HHIIIH", 0xFFFF, 0xFFFF, 0, 0, style, len(controls))
    out += struct.pack("<hhhh", 0, 0, 220, 140)
    out += _se_word_or_ord(None)      # menu
    out += _se_word_or_ord(None)      # class
    out += caption.encode("utf-16-le") + b"\0\0"
    out += struct.pack("<HHBB", pointsize, 400, 0, 1)
    out += typeface.encode("utf-16-le") + b"\0\0"
    while len(out) & 3:
        out += b"\0"
    for (cid, cls, txt, rect) in controls:
        out += struct.pack("<III", 0, 0, 0x50000000)
        out += struct.pack("<hhhh", *rect)
        out += struct.pack("<I", cid)
        out += _se_word_or_ord(cls)
        out += _se_word_or_ord(txt)
        out += struct.pack("<H", 0)
        while len(out) & 3:
            out += b"\0"
    return bytes(out)


def _res_accel(entries):
    out = bytearray()
    for i, (fvirt, key, cmd) in enumerate(entries):
        fv = fvirt | (0x80 if i == len(entries) - 1 else 0)
        out += struct.pack("<HHHH", fv, key, cmd, 0)
    return bytes(out)


def build_resource_pe(resources):
    """
    resources: list of (type_id, res_id, lang, bytes).
    type_id / res_id may be int or str (named entries are legal PE).
    Returns (pe_bytes, info).
    """
    # gather unique tree {type: {id: {lang: payload}}}
    types = {}
    for t, rid, lang, data in resources:
        types.setdefault(t, {}).setdefault(rid, {})[lang] = data

    def sort_key(v):
        return (1, v) if isinstance(v, str) else (0, v)

    dir_area = bytearray()
    data_area = bytearray()
    leaf_fixups = []  # (data_entry_offset, payload_offset_in_data_area)

    def new_dir(n_named, n_id):
        off = len(dir_area)
        dir_area.extend(b"\0" * (16 + 8 * (n_named + n_id)))
        struct.pack_into("<IIHHHH", dir_area, off, 0, 0, 4, 0, n_named, n_id)
        return off

    def put_name(s):
        off = len(dir_area)
        w = s.encode("utf-16-le")
        dir_area.extend(struct.pack("<H", len(s)) + w)
        return off | 0x80000000

    def put_data_entry(payload):
        off = len(dir_area)
        dir_area.extend(struct.pack("<IIII", 0, len(payload), 0, 0))
        leaf_fixups.append((off, len(data_area)))
        data_area.extend(payload)
        return off

    # ---- level 3: language directories (name -> data entry) ----------------
    def lang_level(langs):
        entries = sorted(langs.items())
        off = new_dir(0, len(entries))
        for i, (lang, payload) in enumerate(entries):
            struct.pack_into("<II", dir_area, off + 16 + 8 * i,
                             lang, put_data_entry(payload))
        return off

    # ---- level 2: id directories (name/id -> lang dir | 0x80000000) --------
    def id_level(ids):
        ordered = sorted(ids.items(), key=lambda kv: sort_key(kv[0]))
        named = [e for e in ordered if isinstance(e[0], str)]
        plain = [e for e in ordered if not isinstance(e[0], str)]
        ordered = named + plain  # PE spec: named entries first
        off = new_dir(len(named), len(plain))
        for i, (rid, langs) in enumerate(ordered):
            key = put_name(rid) if isinstance(rid, str) else rid
            struct.pack_into("<II", dir_area, off + 16 + 8 * i,
                             key, lang_level(langs) | 0x80000000)
        return off

    # ---- level 1: type directory, then patch child pointers ---------------
    ordered = sorted(types.items(), key=lambda kv: sort_key(kv[0]))
    named = [e for e in ordered if isinstance(e[0], str)]
    plain = [e for e in ordered if not isinstance(e[0], str)]
    ordered = named + plain
    root = new_dir(len(named), len(plain))
    for i, (t, ids) in enumerate(ordered):
        key = put_name(t) if isinstance(t, str) else t
        struct.pack_into("<II", dir_area, root + 16 + 8 * i,
                         key, id_level(ids) | 0x80000000)

    dir_blob = bytes(dir_area)
    rsrc_payload = bytearray(dir_blob + bytes(data_area))
    # resource data lives after the whole directory blob; patch data RVAs
    for de_off, payload_off in leaf_fixups:
        struct.pack_into("<I", rsrc_payload, de_off,
                         SECTION_ALIGN + len(dir_blob) + payload_off)

    sections = [{"name": ".rsrc", "data": bytes(rsrc_payload),
                 "chars": 0x40000040}]
    pe, secinfo = build_pe(sections, {2: (SECTION_ALIGN, len(rsrc_payload))},
                           characteristics=0x2022)
    return pe, secinfo

__all__ = [
    "build_pe", "build_import_pe", "build_resource_pe",
    "_res_string_block", "_res_menu_classic", "_res_dialogex", "_res_accel",
]
