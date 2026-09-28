#!/usr/bin/env python3
"""Regenerates tests/fixtures/win7_explorer_structure.json from the REAL
explorer.exe.mui using ONLY tests/win32_ref.py (independent of tools/).
Output is structure only: string IDs, menu kinds/ids/depths, dialog geometry,
classes, styles, icon ordinals and text *presence* - never Microsoft text.

    python3 tests/make_win7_fixture.py path/to/explorer.exe.mui
"""
import hashlib, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import win32_ref as w  # noqa: E402


def structure(path):
    data = Path(path).read_bytes()
    res = w.resources(data)
    out = {"source_sha256": hashlib.sha256(data).hexdigest(),
           "string_ids": sorted(w.all_strings(res)), "menus": {}, "dialogs": {}}
    for (t, n, _l), raw in sorted(res.items()):
        if t == w.RT_MENU:
            out["menus"][str(n)] = [[m["depth"], m["kind"], m["id"], bool(m["text"])]
                                    for m in w.parse_menu(raw)]
        elif t == w.RT_DIALOG:
            d = w.parse_dialog(raw)
            out["dialogs"][str(n)] = {
                "ex": d["ex"], "style": d["style"], "rect": list(d["rect"]),
                "controls": [{"id": c["id"], "cls": list(c["cls"]) if isinstance(c["cls"], tuple) else c["cls"],
                              "rect": list(c["rect"]), "style": c["style"],
                              "text": (list(c["text"]) if isinstance(c["text"], tuple)
                                       else ("present" if c["text"] else None))}
                             for c in d["controls"]]}
    return out


if __name__ == "__main__":
    s = structure(sys.argv[1])
    dst = Path(__file__).resolve().parent / "fixtures" / "win7_explorer_structure.json"
    dst.write_text(json.dumps(s, indent=1) + "\n", encoding="utf-8")
    print(dst, len(s["string_ids"]), "strings")
