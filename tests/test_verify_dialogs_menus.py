#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the dialogs/menus validation added to verify_catalog.py."""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.verify_catalog import verify  # noqa: E402


def _write(tmp_path, constraints, catalogs):
    cdir = tmp_path / "catalog"
    cdir.mkdir(exist_ok=True)
    cpath = tmp_path / "con.json"
    cpath.write_text(json.dumps(constraints), encoding="utf-8")
    for name, data in catalogs.items():
        (cdir / f"{name}.json").write_text(json.dumps(data), encoding="utf-8")
    return cdir, [str(cpath)]


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


def _lang(dialogs=None, menus=None, overrides=None, comments=None):
    return {
        "language": {"id": "it"},
        "strings": {},
        "dialogs": dialogs or {},
        "menus": menus or {},
        "maxlen_override": overrides or {},
        "maxlen_override_comment": comments or {},
    }


EN = {"language": {"id": "en"}, "strings": {}}


def test_dup_control_ids_need_suffix_keys(tmp_path):
    good = _lang(dialogs={"6/lang:0409": {
        "title": "Barra",
        "controls": {
            "1105": "&Blocca la barra",
            "65535#1": "P&osizione della barra:",
            "65535#2": "Aspetto",
        }}})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": good})
    assert verify(cdir, cpaths) == 0
    # legacy unsuffixed key for a duplicated id must be an orphan
    bad = _lang(dialogs={"6/lang:0409": {
        "controls": {"65535": "Aspetto"}}})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": bad})
    assert verify(cdir, cpaths) == 1


def test_dialog_accel_collision_is_error(tmp_path):
    bad = _lang(dialogs={"6/lang:0409": {
        "controls": {
            "1105": "&Blocca la barra",       # accel B
            "65535#1": "Posizione della &boh:",    # accel B -> collision
            "65535#2": "Aspetto"}}})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": bad})
    assert verify(cdir, cpaths) == 1


def test_menu_level_accel_collision_is_error(tmp_path):
    bad = _lang(menus={"211/lang:0409": {
        "1/0": "P&roprietà",
        "1/1": "Ap&ri Esplora file"}})  # collision R... use same letter:
    bad["menus"]["211/lang:0409"]["1/1"] = "Apri p&rova"
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": bad})
    assert verify(cdir, cpaths) == 1
    good = _lang(menus={"211/lang:0409": {
        "1/0": "P&roprietà",
        "1/1": "&Apri Esplora file"}})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": good})
    assert verify(cdir, cpaths) == 0


def test_override_requires_comment(tmp_path):
    cat = _lang(
        dialogs={"6/lang:0409": {
            "title": "Titolo molto lungo oltre il budget normale previsto",
            "controls": {}}},
        overrides={"dialog:6/lang:0409:title": 60})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": cat})
    assert verify(cdir, cpaths) == 1  # override without comment
    cat["maxlen_override_comment"] = {
        "dialog:6/lang:0409:title": "titolo finestra, nessun ritaglio"}
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": cat})
    assert verify(cdir, cpaths) == 0


def test_orphan_dialog_and_menu(tmp_path):
    bad = _lang(dialogs={"999/lang:0409": {"controls": {}}},
                menus={"999/lang:0409": {}})
    cdir, cpaths = _write(tmp_path, CON, {"en": EN, "it": bad})
    assert verify(cdir, cpaths) == 1


def test_missing_reference_coverage_flag_behaviour(tmp_path):
    # coverage demanded only when the constraints file opts in
    con2 = dict(CON)
    con2["strings"] = {"42": {"len": 5}}
    con2["require_fallback_coverage"] = True
    cat = _lang(dialogs={"6/lang:0409": {"controls": {
        "1105": "&Blocca", "65535#1": "Po&sizione:", "65535#2": "Aspetto"}}})
    cdir, cpaths = _write(tmp_path, con2, {"en": EN, "it": cat})
    assert verify(cdir, cpaths) == 1  # en misses string 42
    en2 = dict(EN, strings={"42": "hello"})
    cdir, cpaths = _write(tmp_path, con2, {"en": en2, "it": cat})
    assert verify(cdir, cpaths) == 0
