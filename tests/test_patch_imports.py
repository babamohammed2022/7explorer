# -*- coding: utf-8 -*-
"""Tests for tools/patch_imports.py — run with:  python3 tests/run_tests.py"""

import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tools.patch_imports import (  # noqa: E402
    patch_imports, _PE, PEFormatError, pe_checksum,
    IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT,
)
from tests.pebuilder import build_import_pe, build_pe  # noqa: E402


def _dll_names(pe):
    mod = _PE(pe)
    return [mod.read_cstr(mod.rva_to_off(name_rva))
            for _o, name_rva, _t in mod.import_descriptors()]


class ImportPatchTests(unittest.TestCase):
    DLLS = ["SHLWAPI.DLL", "OLE32.DLL", "EXPLORERFRAME.DLL", "KERNEL32.dll"]

    def setUp(self):
        self.pe, self.secinfo = build_import_pe(self.DLLS)

    def test_builder_is_sane(self):
        mod = _PE(self.pe)
        self.assertEqual(mod.machine, 0x8664)
        names = [n.decode() for n in _dll_names(self.pe)]
        self.assertEqual(names, self.DLLS)

    def test_patch_rewrites_only_target_names(self):
        patched, actions = patch_imports(self.pe)
        names = [n.decode() for n in _dll_names(patched)]
        self.assertEqual(
            names, ["wrp64.dll", "wrp64.dll", "wrp64.dll", "KERNEL32.dll"])
        self.assertEqual(len(actions), 4)  # 3 names + checksum line
        # untouched files must not import the wrapper at all
        self.assertNotIn(b"wrp64", self.pe)

    def test_size_is_preserved(self):
        patched, _ = patch_imports(self.pe)
        self.assertEqual(len(patched), len(self.pe))

    def test_deterministic(self):
        a, _ = patch_imports(self.pe)
        b, _ = patch_imports(self.pe)
        self.assertEqual(a, b)
        self.assertEqual(hashlib.sha256(a).hexdigest(),
                         hashlib.sha256(b).hexdigest())

    def test_idempotent(self):
        once, _ = patch_imports(self.pe)
        twice, actions2 = patch_imports(once)
        self.assertEqual(once, twice)
        self.assertEqual(actions2, [])  # nothing left to do

    def test_case_insensitive_match(self):
        pe, _ = build_import_pe(["shlwapi.dll", "ole32.dll"])
        patched, actions = patch_imports(pe)
        names = [n.decode() for n in _dll_names(patched)]
        self.assertEqual(names, ["wrp64.dll", "wrp64.dll"])
        self.assertTrue(any("shlwapi" in a.lower() for a in actions))

    def test_bound_import_cleared(self):
        pe, _ = build_import_pe(self.DLLS, bound_import_names=["SHLWAPI.DLL"])
        mod = _PE(pe)
        self.assertNotEqual(mod.directory(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT),
                            (0, 0))
        patched, actions = patch_imports(pe)
        mod2 = _PE(patched)
        self.assertEqual(mod2.directory(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT),
                         (0, 0))
        self.assertTrue(any("bound" in a for a in actions))
        # clearing twice is a no-op
        patched2, actions2 = patch_imports(patched)
        self.assertNotIn(
            True, ["bound" in a for a in actions2],
            "bound-import clearing must be idempotent")

    def test_checksum_is_valid_and_changes(self):
        patched, actions = patch_imports(self.pe)
        mod = _PE(patched)
        stored = struct.unpack_from("<I", patched, mod.checksum_off)[0]
        buf = bytearray(patched)
        struct.pack_into("<I", buf, mod.checksum_off, 0)
        calc = pe_checksum(buf, mod.checksum_off)
        self.assertEqual(stored, calc,
                         "stored checksum must match the algorithm")
        self.assertTrue(any("CheckSum" in a for a in actions))

    def test_only_three_bytes_regions_change(self):
        """Diff must be: the 3 name slots + bound dir entry + checksum."""
        patched, _ = patch_imports(self.pe)
        mod = _PE(self.pe)
        target_offs = set()
        for _d, name_rva, _t in mod.import_descriptors():
            off = mod.rva_to_off(name_rva)
            name = mod.read_cstr(off)
            if name.upper() in (b"SHLWAPI.DLL", b"OLE32.DLL",
                                b"EXPLORERFRAME.DLL"):
                target_offs.update(range(off, off + len(name) + 1))
        target_offs.add(mod.checksum_off)
        target_offs.update(range(mod.checksum_off + 1, mod.checksum_off + 4))
        diffs = {i for i, (a, b) in enumerate(zip(self.pe, patched)) if a != b}
        self.assertTrue(diffs <= target_offs,
                        f"unexpected diffs at {sorted(diffs - target_offs)}")

    def test_rejects_non_pe(self):
        with self.assertRaises(PEFormatError):
            patch_imports(b"not a pe")
        with self.assertRaises(PEFormatError):
            patch_imports(b"MZ" + b"\0" * 100)
        # 32-bit PE must be refused (the wrapper is x64 only)
        pe32, _ = self._pe32()
        with self.assertRaises(PEFormatError):
            patch_imports(pe32)

    def _pe32(self):
        sections = []
        pe, secinfo = build_pe(sections, {}, characteristics=0x0102)
        # flip machine to I386 and magic to PE32
        pe = bytearray(pe)
        struct.pack_into("<H", pe, 0x80 + 4, 0x14C)
        struct.pack_into("<H", pe, 0x80 + 4 + 20, 0x10B)
        return bytes(pe), secinfo


if __name__ == "__main__":
    unittest.main(verbosity=2)
