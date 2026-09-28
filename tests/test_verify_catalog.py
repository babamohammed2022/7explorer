# -*- coding: utf-8 -*-
"""Tests for tools/verify_catalog.py — run with:  python3 tests/run_tests.py"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.verify_catalog import verify  # noqa: E402

CONSTRAINTS = {
    "source": "fixture",
    "strings": {
        "100": {"len": 20, "accel": "u", "placeholders": []},
        "101": {"len": 22, "accel": "u", "placeholders": ["%1!s!", "%d"]},
        "102": {"len": 10, "accel": None, "placeholders": []},
    },
    "contexts": {
        "menu.x": {"members": ["100", "102"], "coex": True}
    },
}


def write(d: Path, name: str, obj):
    (d / name).write_text(json.dumps(obj, ensure_ascii=False),
                          encoding="utf-8")


class VerifyTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        tdp = Path(self.td.name)
        self.catdir = tdp / "catalog"
        self.catdir.mkdir()
        self.con = tdp / "con.json"
        write(tdp, "con.json", CONSTRAINTS)

    def tearDown(self):
        self.td.cleanup()

    def run_verify(self):
        return verify(self.catdir, [str(self.con)])

    def lang(self, lang_id, strings, **extra):
        obj = {"language": {"id": lang_id, "mui_name": lang_id,
                            "lcid": "0x0409"},
               "strings": strings}
        obj.update(extra)
        return obj

    def test_ok(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "Attach to &menu",
            "101": "Copy %1!s! (%d times)",
            "102": "Exit"}))
        self.assertEqual(self.run_verify(), 0)

    def test_missing_en_coverage(self):
        write(self.catdir, "en.json", self.lang("en", {"100": "a"}))
        self.assertEqual(self.run_verify(), 1)

    def test_no_fallback_language(self):
        write(self.catdir, "it.json", self.lang("it", {"100": "x"}))
        self.assertEqual(self.run_verify(), 1)

    def test_orphan_id(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "A", "101": "B %1!s! %d", "102": "C", "999": "orphan"}))
        self.assertEqual(self.run_verify(), 1)

    def test_placeholder_mismatch(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "A",
            "101": "Copy %1!s! (%u times)",  # %d -> %u is a type change
            "102": "C"}))
        self.assertEqual(self.run_verify(), 1)

    def test_placeholder_order_matters(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "A", "101": "%d then %1!s!", "102": "C"}))
        self.assertEqual(self.run_verify(), 1)

    def test_two_accelerators_rejected(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "&A &B", "101": "x %1!s! %d", "102": "C"}))
        self.assertEqual(self.run_verify(), 1)

    def test_context_accel_uniqueness(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "Attach to &menu",
            "101": "x %1!s! %d",
            "102": "&menu exit"}))  # same 'm' as 100 in coex context
        self.assertEqual(self.run_verify(), 1)

    def test_length_budget(self):
        write(self.catdir, "en.json", self.lang("en", {
            "100": "A",
            "101": "x %1!s! %d",
            "102": "x" * 60}))  # ref len 10, budget 20
        self.assertEqual(self.run_verify(), 1)

    def test_length_override_requires_comment(self):
        write(self.catdir, "en.json", self.lang(
            "en",
            {"100": "A", "101": "x %1!s! %d", "102": "x" * 30},
            maxlen_override={"102": 40}))
        self.assertEqual(self.run_verify(), 1)
        write(self.catdir, "en.json", self.lang(
            "en",
            {"100": "A", "101": "x %1!s! %d", "102": "x" * 30},
            maxlen_override={"102": 40},
            maxlen_override_comment={"102": "long infotip, verified on UI"}))
        self.assertEqual(self.run_verify(), 0)

    def test_real_catalog_passes(self):
        repo = Path(__file__).resolve().parent.parent
        code = verify(repo / "localization" / "catalog",
                      [str(repo / "localization" / "constraints"
                           / "shell32.dll.constraints.json")])
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
