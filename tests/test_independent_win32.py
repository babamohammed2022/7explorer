"""Independent end-to-end check of the generated resources.

The generator (tools/build_resources.py) builds the payloads; they are packed
into a resource-only PE and read back with tests/win32_ref.py, a Win32 reader
written from the Microsoft specs that shares no code with tools/.

The EXPECTED structure comes from the real Win7 explorer.exe.mui:
  * if the real file is available (env EX7_REF_MUI, or
    localization/explorer.exe.mui) it is parsed directly with win32_ref;
  * otherwise from tests/fixtures/win7_explorer_structure.json, which was
    produced from that same file by tests/make_win7_fixture.py (win32_ref
    only, structure only, no Microsoft text).
Expected TEXT is the catalog text for that ID/key (our own wording).
"""
import json
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))

import win32_ref as w  # noqa: E402
from make_win7_fixture import structure  # noqa: E402
from pebuilder import build_resource_pe  # noqa: E402
from tools.build_resources import build_all  # noqa: E402

REAL_SHA = "4cc514a7d9afae763cdd21932ee722ae4a787c968bab971c3b1d30044151cfe3"


def reference():
    for cand in (os.environ.get("EX7_REF_MUI"),
                 ROOT / "localization" / "explorer.exe.mui"):
        if cand and Path(cand).is_file():
            s = structure(cand)
            if s["source_sha256"] == REAL_SHA:
                return s, "real file"
    fx = json.loads((HERE / "fixtures" / "win7_explorer_structure.json")
                    .read_text(encoding="utf-8"))
    assert fx["source_sha256"] == REAL_SHA
    return fx, "fixture"


REF, REF_ORIGIN = reference()
CAT = {k: json.loads((ROOT / "localization" / "catalog" / f"{k}.json")
                     .read_text(encoding="utf-8")) for k in ("en", "it")}
TPL = json.loads((ROOT / "localization" / "templates" /
                  "explorer.exe.templates.json").read_text(encoding="utf-8"))
LANG = 0x409


def generated(lang):
    ids = set(REF["string_ids"])
    out = build_all({lang: CAT[lang]}, TPL, ids)
    res = [(p["type"], p["resId"], LANG, p["payload"]) for p in out[lang]]
    pe, _info = build_resource_pe(res)
    return w.resources(pe)


def menu_text_keys(flat):
    """Catalog keys 'level/index' for a depth-first flat menu (win32_ref).

    Levels are numbered in order of popup appearance (top bar = 0); the
    index is the position inside the parent's item list, separators counted.
    """
    keys = []
    stack = [[0, 0]]  # [level number, next index] per open depth
    next_level = 1
    for m in flat:
        del stack[m["depth"] + 1:]
        lvl, idx = stack[m["depth"]]
        keys.append(f"{lvl}/{idx}")
        stack[m["depth"]][1] += 1
        if m["kind"] == "popup":
            stack.append([next_level, 0])
            next_level += 1
    return keys


class IndependentStrings(unittest.TestCase):
    SAMPLE = [300, 320, 336, 352, 511, 857, 858, 8272, 19653]

    def test_reference_ids(self):
        ids = REF["string_ids"]
        self.assertEqual(len(ids), 161, REF_ORIGIN)
        self.assertEqual((min(ids), max(ids)), (300, 19653))
        for sid in (306, 859, 19654, 8273):
            self.assertNotIn(sid, ids)

    def test_loadstring_sample(self):
        for lang in ("en", "it"):
            res = generated(lang)
            for sid in self.SAMPLE:
                self.assertEqual(w.load_string(res, sid),
                                 CAT[lang]["strings"][str(sid)],
                                 f"{lang} LoadString({sid})")

    def test_loadstring_every_id_and_no_extras(self):
        for lang in ("en", "it"):
            res = generated(lang)
            got = w.all_strings(res)
            self.assertEqual(sorted(got), REF["string_ids"], lang)
            for sid in REF["string_ids"]:
                self.assertEqual(got[sid], CAT[lang]["strings"][str(sid)],
                                 f"{lang} id {sid}")

    def test_user_reported_symptoms(self):
        res = generated("it")
        self.assertEqual(w.load_string(res, 8234), "Pannello di controllo")
        self.assertEqual(w.load_string(res, 856), "Personalizza...")
        self.assertEqual(w.load_string(res, 7021), "Guida e supporto")
        self.assertNotEqual(w.load_string(res, 858), "Sto finendo")


class IndependentMenus(unittest.TestCase):
    def test_structure_matches_real(self):
        for lang in ("en", "it"):
            res = generated(lang)
            got_ids = sorted(n for (t, n, _l) in res if t == w.RT_MENU)
            self.assertEqual(got_ids, sorted(map(int, REF["menus"])))
            for mid, ref in REF["menus"].items():
                flat = w.parse_menu(res[(w.RT_MENU, int(mid), LANG)])
                got = [[m["depth"], m["kind"], m["id"], bool(m["text"])]
                       for m in flat]
                self.assertEqual(got, ref, f"{lang} menu {mid}")
                texts = CAT[lang]["menus"].get(f"{mid}/lang:0409", {})
                for key, m in zip(menu_text_keys(flat), flat):
                    if key in texts:
                        self.assertEqual(m["text"], texts[key],
                                         f"{lang} menu {mid} {key}")

    def test_expected_cmds(self):
        want = {205: [408, 421, 403, 405, 404, 407, 416, 420, 424, 413],
                12000: [403, 405, 404, 65493, 65492, 65491]}
        for mid, cmds in want.items():
            ref = [e[2] for e in REF["menus"][str(mid)] if e[1] == "item"]
            self.assertEqual(ref, cmds, f"reference menu {mid}")
            res = generated("it")
            got = [m["id"] for m in w.parse_menu(res[(w.RT_MENU, mid, LANG)])
                   if m["kind"] == "item"]
            self.assertEqual(got, cmds, f"generated menu {mid}")
        self.assertEqual(len(REF["menus"]["205"]), 14)  # popup + 13


class IndependentDialogs(unittest.TestCase):
    def test_geometry_ids_classes(self):
        for lang in ("en", "it"):
            res = generated(lang)
            for did, ref in REF["dialogs"].items():
                d = w.parse_dialog(res[(w.RT_DIALOG, int(did), LANG)])
                self.assertTrue(d["ex"], f"{lang} dialog {did} not DIALOGEX")
                self.assertEqual(d["style"], ref["style"])
                self.assertEqual(list(d["rect"]), ref["rect"])
                self.assertEqual(len(d["controls"]), len(ref["controls"]))
                for c, rc in zip(d["controls"], ref["controls"]):
                    where = f"{lang} dialog {did} ctl {rc['id']}"
                    self.assertEqual(c["id"], rc["id"], where)
                    self.assertEqual(list(c["rect"]), rc["rect"], where)
                    self.assertEqual(c["style"], rc["style"], where)
                    cls = list(c["cls"]) if isinstance(c["cls"], tuple) else c["cls"]
                    self.assertEqual(cls, rc["cls"], where)
                    if isinstance(rc["text"], list):  # icon ordinal
                        self.assertEqual(list(c["text"]), rc["text"], where)
                    elif rc["text"] == "present":
                        self.assertTrue(c["text"], where)

    def test_syslinks_have_anchor(self):
        for lang in ("en", "it"):
            res = generated(lang)
            for did, ref in REF["dialogs"].items():
                d = w.parse_dialog(res[(w.RT_DIALOG, int(did), LANG)])
                for c in d["controls"]:
                    if c["cls"] == "SysLink":
                        self.assertRegex(c["text"], r"<A>.+</A>")


if __name__ == "__main__":
    unittest.main()
