# -*- coding: utf-8 -*-
"""Tests for dialogs/menus validation in verify_catalog.py.
Run with:  python3 tests/run_tests.py"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.verify_catalog import verify  # noqa: E402

CON = {
    "source": "unit test",
    "require_fallback_coverage": False,
    "strings": {},
    "contexts": {},
    "dialogs": {
        "6/lang:0409": {
            "title_len": 7,
            "controls": [
                {"id": 1105, "class": "BUTTON", "len": 16, "accel": "L"},
                {"id": 65535, "class": "STATIC", "len": 27, "accel": "T"},
                {"id": 65535, "class": "BUTTON", "len": 18},
            ]
        }
    },
    "menus": {
        "211/lang:0409": {
            "levels": [
                [{"popup": True, "popup_level": 1}],
                [
                    {"popup": False, "cmd": 1, "len": 10, "accel": "r"},
                    {"popup": False, "cmd": 2, "len": 13, "accel": "x"},
                ],
            ]
        }
    },
}

EN = {"language": {"id": "en"}, "strings": {}}


def _lang(dialogs=None, menus=None, overrides=None, comments=None):
    return {
        "language": {"id": "it"},
        "strings": {},
        "dialogs": dialogs or {},
        "menus": menus or {},
        "maxlen_override": overrides or {},
        "maxlen_override_comment": comments or {},
    }


class DialogMenuTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        tdp = Path(self.td.name)
        self.catdir = tdp / "catalog"
        self.catdir.mkdir()
        self.con = tdp / "con.json"
        self.con.write_text(json.dumps(CON), encoding="utf-8")

    def tearDown(self):
        self.td.cleanup()

    def run_verify(self, cat):
        for name, data in {"en": EN, "it": cat}.items():
            (self.catdir / f"{name}.json").write_text(
                json.dumps(data, ensure_ascii=False), encoding="utf-8")
        return verify(self.catdir, [str(self.con)])

    def test_dup_control_ids_need_suffix_keys(self):
        good = _lang(dialogs={"6/lang:0409": {
            "title": "Barra",
            "controls": {
                "1105": "&Blocca la barra",
                "65535#1": "P&osizione della barra:",
                "65535#2": "Aspetto"}}})
        self.assertEqual(self.run_verify(good), 0)
        # legay unsuffixed key for a duplicated id must be orphan
        bad = _lang(dialogs={"6/lang:0409": {
            "controls": {"65535": "Aspetto"}}})
        self.assertEqual(self.run_verify(bad), 1)

    def test_dialog_accel_collision_is_error(self):
        bad = _lang(dialogs={"6/lang:0409": {
            "controls": {
                "1105": "&Blocca la barra",
                "65535#1": "Posizione della &boh:",
                "65535#2": "Aspetto"}}})
        self.assertEqual(self.run_verify(bad), 1)

    def test_menu_level_accel_collision_is_error(self):
        bad = _lang(menus={"211/lang:0409": {
            "1/0": "P&roprietà",
            "1/1": "Apri p&rova"}})
        self.assertEqual(self.run_verify(bad), 1)
        good = _lang(menus={"211/lang:0409": {
            "1/0": "P&roprietà",
            "1/1": "&Apri Esplora file"}})
        self.assertEqual(self.run_verify(good), 0)

    def test_override_requires_comment(self):
        cat = _lang(
            dialogs={"6/lang:0409": {
                "title": "Titolo molto lungo oltre il budget normale",
                "controls": {}}},
            overrides={"dialog:6/lang:0409:title": 60})
        self.assertEqual(self.run_verify(cat), 1)
        cat["maxlen_override_comment"] = {
            "dialog:6/lang:0409:title": "titolo finestra, nessun ritaglio"}
        self.assertEqual(self.run_verify(cat), 0)

    def test_orphan_dialog_and_menu(self):
        bad = _lang(dialogs={"999/lang:0409": {"controls": {}}},
                    menus={"999/lang:0409": {}})
        self.assertEqual(self.run_verify(bad), 1)

    def test_fallback_coverage_flag(self):
        con2 = dict(CON)
        con2["strings"] = {"42": {"len": 5}}
        con2["require_fallback_coverage"] = True
        self.con.write_text(json.dumps(con2), encoding="utf-8")
        cat = _lang()
        self.assertEqual(self.run_verify(cat), 1)  # en misses 42
        en2 = dict(EN, strings={"42": "hello"})
        (self.catdir / "en.json").write_text(
            json.dumps(en2, ensure_ascii=False), encoding="utf-8")
        (self.catdir / "it.json").write_text(
            json.dumps(cat, ensure_ascii=False), encoding="utf-8")
        self.assertEqual(verify(self.catdir, [str(self.con)]), 0)


if __name__ == "__main__":
    unittest.main()
