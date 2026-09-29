# -*- coding: utf-8 -*-
"""Tests for tools/build_resources.py (projection: catalog+templates -> PE
resource payloads) and for the PE-side checker logic. Run with:
    python3 tests/run_tests.py"""

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
ROOT = Path(__file__).resolve().parent.parent

from tools.build_resources import (  # noqa: E402
    build_string_block, build_menu, build_dialog, build_accelerators,
    build_all, validate_menu, validate_dialog, compare_templates,
    BuildError, RT_STRING, RT_MENU, RT_DIALOG,
)
from tools.analyze_mui import (  # noqa: E402
    parse_string_table, parse_menu, parse_dialog, parse_accelerators,
)


class TestStringBlocks(unittest.TestCase):
    def test_block_id_mapping(self):
        # Win32: block = (id >> 4) + 1 = 19 holds ids 288..303, slot = id & 15.
        payload = build_string_block(19, {300: "hello", 303: "world"})
        back = parse_string_table(payload, 19)
        self.assertEqual(back.get(300), "hello")
        self.assertEqual(back.get(303), "world")
        self.assertNotIn(299, back)
        self.assertNotIn(301, back)
        # byte-level check, independent of parse_string_table: slot 12
        off = 0
        for i in range(16):
            ln = struct.unpack_from("<H", payload, off)[0]
            if i == 12:
                self.assertEqual(payload[off + 2:off + 2 + 2 * ln]
                                 .decode("utf-16-le"), "hello")
            off += 2 + 2 * ln

    def test_utf16_encoding_and_termination(self):
        s = "àèéìòù €"
        payload = build_string_block(1, {1: s})
        back = parse_string_table(payload, 1)
        self.assertEqual(back[1], s)

    def test_empty_slots(self):
        payload = build_string_block(1, {})
        self.assertEqual(parse_string_table(payload, 1), {})


class TestMenuBuild(unittest.TestCase):
    def _classic(self):
        return {"version": 0, "levels": [
            [{"flags": 0x90, "id": 0, "popup": True,
              "popup_level": 1, "text_meta": {"len": 5, "accel": "F",
                                              "used": True}}],
            [{"flags": 0x00, "id": 100, "cmd": 100, "popup": False,
              "text_meta": {"len": 3, "accel": "a", "used": True}},
             {"flags": 0x80, "id": 101, "cmd": 101, "popup": False,
              "text_meta": {"len": 3, "accel": "b", "used": True}}],
        ]}

    def test_classic_roundtrip(self):
        tpl = self._classic()
        payload = build_menu(tpl, {"0/0": "&File", "1/0": "&aa",
                                   "1/1": "&bb"})
        m = parse_menu(payload)
        self.assertEqual(m["version"], 0)
        self.assertEqual(len(m["levels"]), 2)
        self.assertEqual(m["levels"][1][0]["cmd"], 100)
        self.assertEqual(m["levels"][1][1]["cmd"], 101)
        self.assertEqual(m["levels"][1][0]["text"], "&aa")

    def test_extended_roundtrip(self):
        tpl = {"version": 1, "levels": [
            [{"dwType": 0, "dwState": 0, "cmd": 0, "bResInfo": 0x81,
              "help_id": 0, "popup": True, "popup_level": 1,
              "text_meta": {"len": 4, "accel": None, "used": True}}],
            [{"dwType": 0, "dwState": 0, "cmd": 6601, "bResInfo": 0,
              "popup": False, "text_meta": {"len": 2, "accel": "S",
                                            "used": True}},
             {"dwType": 0x800, "dwState": 0, "cmd": 0xFFFFFFFF,
              "bResInfo": 0, "popup": False,
              "text_meta": {"len": 0, "accel": None, "used": False}},
             {"dwType": 0, "dwState": 0, "cmd": 6604, "bResInfo": 0x80,
              "popup": False, "text_meta": {"len": 2, "accel": "x",
                                            "used": True}}],
        ]}
        payload = build_menu(tpl, {"0/0": "Help", "1/0": "&S1",
                                   "1/2": "&x1"})
        m = parse_menu(payload)
        self.assertEqual(m["version"], 1)
        self.assertEqual(len(m["levels"]), 2)
        self.assertEqual(m["levels"][1][0]["cmd"], 6601)
        self.assertEqual(m["levels"][1][2]["cmd"], 6604)

    def test_validate_menu_cmd_mismatch_detected(self):
        tpl = self._classic()
        payload = bytearray(build_menu(tpl, {"0/0": "&F", "1/0": "a",
                                             "1/1": "b"}))
        bad_tpl = self._classic()
        bad_tpl["levels"][1][0]["cmd"] = 999
        with self.assertRaises(BuildError):
            validate_menu(bytes(payload), bad_tpl)


class TestDialogBuild(unittest.TestCase):
    def _tpl(self):
        return {
            "ex": True, "help_id": None, "ex_style": 0,
            "style": 0x50000048, "rect": [0, 0, 200, 100],
            "menu": {"kind": "null", "value": None},
            "window_class": {"kind": "null", "value": None},
            "title": {"len": 5, "accel": None},
            "font": {"points": 8, "weight": 400, "italic": 0, "charset": 0,
                     "face": {"kind": "string", "value": "MS Shell Dlg"}},
            "controls": [
                {"id": 1001, "help_id": None, "ex_style": 0,
                 "style": 0x50010003, "rect": [10, 10, 80, 12],
                 "class": {"kind": "atom", "value": 0x0080},
                 "text": {"len": 2, "accel": "a"}, "extra_hex": ""},
                {"id": 0xFFFF, "help_id": None, "ex_style": 0,
                 "style": 0x50000000, "rect": [10, 30, 100, 10],
                 "class": {"kind": "atom", "value": 0x0082},
                 "text": {"len": 2, "accel": None}, "extra_hex": ""},
            ],
        }

    def test_dialogex_roundtrip(self):
        tpl = self._tpl()
        payload = build_dialog(tpl, "Title", {"1001": "&aa",
                                              "65535": "bb"})
        d = parse_dialog(payload)
        self.assertTrue(d["ex"])
        self.assertEqual(d["rect"], [0, 0, 200, 100])
        self.assertEqual(len(d["controls"]), 2)
        ids = [c["id"] for c in d["controls"]]
        self.assertEqual(ids, [1001, 0xFFFF])

    def test_duplicate_ctl_ids_occurrence_keys(self):
        tpl = self._tpl()
        tpl["controls"].append(dict(tpl["controls"][1]))
        tpl["controls"][-1] = dict(tpl["controls"][1])
        payload = build_dialog(tpl, "Title",
                               {"65535#1": "uno", "65535#2": "due"})
        d = parse_dialog(payload)
        texts = [c.get("text") for c in d["controls"] if c["id"] == 0xFFFF]
        self.assertEqual(texts, ["uno", "due"])

    def test_validate_dialog_rect_mismatch_detected(self):
        tpl = self._tpl()
        payload = build_dialog(tpl, "Title", {"1001": "x", "65535": "y"})
        bad = self._tpl()
        bad["rect"] = [0, 0, 300, 100]
        with self.assertRaises(BuildError):
            validate_dialog(payload, bad, {})


class TestAccelerators(unittest.TestCase):
    def test_last_row_flag(self):
        rows = [{"fVirt": 3, "key": 9, "cmd": 41008},
                {"fVirt": 19, "key": 77, "cmd": 419}]
        payload = build_accelerators(rows)
        back = parse_accelerators(payload)
        self.assertEqual(len(back), 2)
        self.assertEqual(back[-1]["cmd"], 419)


class TestFullRepoBuild(unittest.TestCase):
    def test_repo_payloads_build_and_validate(self):
        cat = {}
        for f in (ROOT / "localization" / "catalog").glob("*.json"):
            d = json.loads(f.read_text(encoding="utf-8"))
            cat[d["language"]["id"]] = d
        tpl = json.loads((ROOT / "localization" / "templates" /
                          "explorer.exe.templates.json").read_text(
            encoding="utf-8"))
        con = json.loads((ROOT / "localization" / "constraints" /
                          "explorer.exe.constraints.json").read_text(
            encoding="utf-8"))
        res = build_all(cat, tpl, set(int(k) for k in con["strings"]))
        self.assertIn("en", res)
        self.assertIn("it", res)
        # en and it must expose the SAME (type,id) set (fallback parity)
        en_keys = sorted((b["type"], b["resId"]) for b in res["en"])
        it_keys = sorted((b["type"], b["resId"]) for b in res["it"])
        self.assertEqual(en_keys, it_keys)
        types = {t for t, _ in en_keys}
        self.assertEqual(types, {RT_MENU, RT_DIALOG, RT_STRING,
                                 9})  # accel = 9

    def test_compare_templates_self_is_clean(self):
        p = str(ROOT / "localization" / "templates" /
                "explorer.exe.templates.json")
        self.assertEqual(compare_templates(p, p), 0)


if __name__ == "__main__":
    unittest.main()
