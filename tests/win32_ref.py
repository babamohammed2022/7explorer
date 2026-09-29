"""Independent Win32 resource reader used ONLY by tests.

Written from the Microsoft specifications, deliberately sharing NO code with
tools/analyze_mui.py or tools/build_resources.py, so that a bug in the
generator cannot be mirrored by the checker.

Specs:
  * STRINGTABLE: block = (id >> 4) + 1, slot = id & 15; 16 length-prefixed
    UTF-16LE strings per block (LoadString semantics).
  * MENUITEMTEMPLATE (classic, wVersion 0): WORD mtOption; WORD mtID (absent
    when MF_POPUP 0x10); WCHAR mtString[]. MF_END = 0x80.
  * MENUEX_TEMPLATE_ITEM (wVersion 1): DWORD dwType, DWORD dwState, DWORD uId,
    WORD wFlags (bResInfo), WCHAR szText[], pad to DWORD, DWORD dwHelpId only
    when wFlags & 0x01 (popup). 0x80 = last item.
  * DLGTEMPLATEEX / DLGITEMTEMPLATEEX (and the classic DLGTEMPLATE).
"""
import struct

RT_MENU, RT_DIALOG, RT_STRING = 4, 5, 6


def _rsrc(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (96 if magic == 0x10B else 112)
    rva = struct.unpack_from("<I", data, dd + 2 * 8)[0]
    sec = opt + optsz
    for i in range(nsec):
        s = sec + 40 * i
        vsz, va, rawsz, rawp = struct.unpack_from("<IIII", data, s + 8)
        if va <= rva < va + max(vsz, rawsz):
            return rva, (lambda r, va=va, rawp=rawp: r - va + rawp)
    raise ValueError("no .rsrc")


def resources(data):
    """{(type, name, lang): bytes} for integer types/names."""
    root_rva, to_off = _rsrc(data)
    root = to_off(root_rva)
    out = {}

    def entries(off):
        nn, ni = struct.unpack_from("<HH", data, off + 12)
        for k in range(nn + ni):
            n, e = struct.unpack_from("<II", data, off + 16 + 8 * k)
            yield n, e

    for t, e1 in entries(root):
        if t & 0x80000000:
            continue  # named types (e.g. "MUI") not needed
        for n, e2 in entries(root + (e1 & 0x7FFFFFFF)):
            if n & 0x80000000:
                continue
            for l, e3 in entries(root + (e2 & 0x7FFFFFFF)):
                de = root + e3
                drva, size = struct.unpack_from("<II", data, de)
                o = to_off(drva)
                out[(t, n, l)] = data[o:o + size]
    return out


def load_string(res, sid):
    """Emulates LoadStringW: returns None when the string is absent/empty."""
    block, slot = (sid >> 4) + 1, sid & 15
    raw = next((v for (t, n, _l), v in res.items()
                if t == RT_STRING and n == block), None)
    if raw is None:
        return None
    off = 0
    for i in range(16):
        ln = struct.unpack_from("<H", raw, off)[0]
        off += 2
        if i == slot:
            return raw[off:off + 2 * ln].decode("utf-16-le") if ln else None
        off += 2 * ln
    return None


def all_strings(res):
    out = {}
    for (t, n, _l), raw in res.items():
        if t != RT_STRING:
            continue
        off = 0
        for i in range(16):
            ln = struct.unpack_from("<H", raw, off)[0]
            off += 2
            if ln:
                out[(n - 1) * 16 + i] = raw[off:off + 2 * ln].decode("utf-16-le")
            off += 2 * ln
    return out


def _sz(raw, off):
    end = off
    while raw[end:end + 2] != b"\0\0":
        end += 2
    return raw[off:end].decode("utf-16-le"), end + 2


def parse_menu(raw):
    """Flat depth-first list of dicts: depth, kind(item|sep|popup), id, text."""
    ver, hdr = struct.unpack_from("<HH", raw, 0)
    out = []
    if ver == 0:
        off = 4 + hdr

        def level(depth):
            nonlocal off
            while True:
                fl = struct.unpack_from("<H", raw, off)[0]
                off += 2
                if fl & 0x10:
                    txt, off = _sz(raw, off)
                    out.append(dict(depth=depth, kind="popup", id=None, text=txt))
                    level(depth + 1)
                else:
                    cid = struct.unpack_from("<H", raw, off)[0]
                    off += 2
                    txt, off = _sz(raw, off)
                    kind = "sep" if (cid == 0 and txt == "") or fl & 0x800 else "item"
                    out.append(dict(depth=depth, kind=kind, id=cid, text=txt))
                if fl & 0x80:
                    return
        level(0)
    elif ver == 1:
        off = 4 + hdr  # wOffset counts from end of the two header WORDs
        out_help = struct.unpack_from("<I", raw, 4)[0] if hdr >= 4 else 0

        def level(depth):
            nonlocal off
            while True:
                dt, ds, uid, wf = struct.unpack_from("<IIIH", raw, off)
                off += 14
                txt, off = _sz(raw, off)
                off = (off + 3) & ~3
                if wf & 0x01:
                    off += 4  # dwHelpId
                    out.append(dict(depth=depth, kind="popup", id=uid, text=txt))
                    level(depth + 1)
                else:
                    kind = "sep" if dt & 0x800 else "item"
                    out.append(dict(depth=depth, kind=kind, id=uid, text=txt))
                if wf & 0x80:
                    return
        level(0)
        del out_help
    else:
        raise ValueError("menu version %d" % ver)
    return out


def _sz_or_ord(raw, off):
    w = struct.unpack_from("<H", raw, off)[0]
    if w == 0xFFFF:
        return ("ord", struct.unpack_from("<H", raw, off + 2)[0]), off + 4
    if w == 0:
        return None, off + 2
    s, off = _sz(raw, off)
    return s, off


def parse_dialog(raw):
    ex = struct.unpack_from("<HH", raw, 0) == (1, 0xFFFF)
    if ex:
        _h, exs, style, n = struct.unpack_from("<IIIH", raw, 4)
        off = 18
    else:
        style, exs, n = struct.unpack_from("<IIH", raw, 0)
        off = 10
    rect = struct.unpack_from("<hhhh", raw, off)
    off += 8
    _menu, off = _sz_or_ord(raw, off)
    _cls, off = _sz_or_ord(raw, off)
    title, off = _sz(raw, off)
    if style & 0x40:  # DS_SETFONT
        off += 6 if ex else 2
        _face, off = _sz(raw, off)
    ctrls = []
    for _ in range(n):
        off = (off + 3) & ~3
        if ex:
            _h, cexs, cst = struct.unpack_from("<III", raw, off)
            crect = struct.unpack_from("<hhhh", raw, off + 12)
            cid = struct.unpack_from("<I", raw, off + 20)[0]
            off += 24
        else:
            cst, cexs = struct.unpack_from("<II", raw, off)
            crect = struct.unpack_from("<hhhh", raw, off + 8)
            cid = struct.unpack_from("<H", raw, off + 16)[0]
            off += 18
        cls, off = _sz_or_ord(raw, off)
        txt, off = _sz_or_ord(raw, off)
        extra = struct.unpack_from("<H", raw, off)[0]
        off += 2 + extra
        ctrls.append(dict(id=cid, cls=cls, rect=crect, style=cst, text=txt))
    return dict(ex=ex, style=style, rect=rect, title=title, controls=ctrls)
