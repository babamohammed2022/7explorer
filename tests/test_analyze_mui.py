import struct
# -*- coding: utf-8 -*-
"""Tests for tools/analyze_mui.py — run with:  python3 tests/run_tests.py"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.analyze_mui import (  # noqa: E402
    build_report, build_constraints, extract_placeholders, extract_accel,
    norm_token, parse_string_table, parse_menu, parse_dialog,
)
from tests.pebuilder import (  # noqa: E402
    build_resource_pe, _res_string_block, _res_menu_classic,
    _res_dialogex, _res_accel,
)


class TokenTests(unittest.TestCase):
    def test_placeholders(self):
        self.assertEqual(extract_placeholders("a %s b"), ["%s"])
        self.assertEqual(
            extract_placeholders("%1!s! — %2 of %I64u %%"),
            ["%1!s!", "%2", "%I64u", "%%"])
        with self.assertRaises(ValueError):
            extract_placeholders("dangling % end")
        self.assertEqual(norm_token("%1!S!"), "%1!s!")

    def test_accels(self):
        self.assertEqual(extract_accel("Open &File"), "F")
        self.assertEqual(extract_accel("R&D && ?"), "D")  # 1st '&' wins; '&&' literal
        self.assertIsNone(extract_accel("plain"))
        self.assertIsNone(extract_accel("A&&B"))
        with self.assertRaises(ValueError):
            extract_accel("&Two &accels")


class ReportTests(unittest.TestCase):
    LANG = 0x409

    def _fixture(self):
        # Win32: block = (id >> 4) + 1, slot = id & 15 -> block 337 covers
        # 5376..5391 and slot i holds id 5376+i. Real shell32.dll ids used
        # by StartMenuPin.cpp: 5381/5382/5384/5385 (0x1505/06/08/09).
        block_id = 337
        strings = [""] * 16
        strings[5] = "Attach to the Start Men&u"     # id 5381
        strings[6] = "Detach from the Start Men&u"   # id 5382
        strings[8] = "Adds %1!s! to the Start menu"  # id 5384
        strings[9] = "Removes %1!s! from the Start menu"  # id 5385

        # Win32: a popup's body follows the popup item immediately.
        menu = (struct.pack("<HH", 0, 0)
                + struct.pack("<H", 0x10) + "&Toolbars".encode("utf-16-le")
                + b"\0\0"
                + _res_menu_classic([(0, "&Alpha", 201),
                                     (0, "&Beta", 202)], header=False)
                + struct.pack("<HH", 0x80, 101)
                + "E&xit".encode("utf-16-le") + b"\0\0")

        dlg = _res_dialogex(
            "Opt&ions",
            [(1308, 0x0082, "&Label", (8, 7, 80, 12)),   # STATIC
             (1, 0x0080, "OK", (164, 117, 50, 16)),      # BUTTON
             ])

        accels = _res_accel([(0x09, ord("N"), 400), (0x09, ord("E"), 401)])

        resources = [
            (6, block_id, self.LANG, _res_string_block(strings)),
            (4, 205, self.LANG, menu),
            (5, 311, self.LANG, dlg),
            (9, 100, self.LANG, accels),
            (10, "MUI", self.LANG, b"\xcd\xfe\xcd\xfe" + b"\0" * 32),
        ]
        return build_resource_pe(resources)

    def test_report_end_to_end(self):
        pe, _info = self._fixture()
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "fixture.exe.mui"
            p.write_bytes(pe)
            rep = build_report(str(p), with_strings=True)
            con = build_constraints(rep)

        self.assertTrue(rep["has_mui_resource"])
        self.assertEqual(rep["time_date_stamp"], "0x4CE7A144")

        s = rep["strings"]
        self.assertEqual(s["5381"]["text"], "Attach to the Start Men&u")
        self.assertEqual(s["5381"]["accel"], "u")
        self.assertEqual(s["5381"]["placeholders"], [])
        self.assertEqual(s["5384"]["placeholders"], ["%1!s!"])

        m = rep["menus"]["205/lang:0409"]
        self.assertEqual(m["version"], 0)
        self.assertEqual(len(m["levels"][0]), 2)
        popup = m["levels"][0][0]
        self.assertEqual(popup["popup"], 1)
        self.assertEqual(m["levels"][popup["popup"]][1]["text"], "&Beta")
        self.assertEqual(m["levels"][popup["popup"]][0]["cmd"], 201)

        d = rep["dialogs"]["311/lang:0409"]
        self.assertTrue(d["ex"])
        self.assertEqual(d["title_accel"], "i")
        self.assertEqual(d["controls"][0]["class"], "STATIC")
        self.assertEqual(d["controls"][1]["rect"], [164, 117, 50, 16])
        self.assertEqual(d["controls"][0]["accel"], "L")

        a = rep["accelerators"]["100/lang:0409"]
        self.assertEqual([x["cmd"] for x in a], [400, 401])
        self.assertTrue(a[-1]["last"])

        # constraints contain no reference text at all
        self.assertNotIn("Attach to the", json.dumps(con))
        self.assertNotIn("Opt", json.dumps(con["dialogs"]))
        self.assertIn("5381", con["strings"])
        self.assertIn("205/lang:0409", con["menus"])
        self.assertIn("311/lang:0409", con["dialogs"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
