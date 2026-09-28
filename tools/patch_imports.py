#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_imports.py — Reference implementation (cross-platform) of the
SHLWAPI.DLL / OLE32.DLL / EXPLORERFRAME.DLL -> wrp64.dll import patch.

This replaces the manual CFF Explorer step described in README.md
("Manual Installation/Patching", Step 2).

Properties:
  * deterministic: same input bytes -> same output bytes, always
  * idempotent: patching an already patched file is a no-op
  * conservative: only the DLL-name strings of the import descriptors
    (and the bound-import directory, which is cleared) are touched;
    no thunk, descriptor or relocation data is altered
  * safe: refuses to patch anything that is not a well-formed PE32+
    for AMD64, or whose import directory is malformed

The C++ installer (installer/ex7selfcontained) ports this logic 1:1 and
must byte-match this implementation on the same input file (see
docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md, task 2).

Tested by tests/test_patch_imports.py (runs on Linux and Windows).
"""

from __future__ import annotations

import hashlib
import struct
import sys


# ---------------------------------------------------------------------------
# PE parsing helpers (deliberately dependency-free, read-only)
# ---------------------------------------------------------------------------

IMAGE_FILE_MACHINE_AMD64 = 0x8664
PE32PLUS_MAGIC = 0x20B
IMAGE_DIRECTORY_ENTRY_IMPORT = 1
IMAGE_DIRECTORY_ENTRY_SECURITY = 4
IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT = 11

DOS_MAGIC = b"MZ"
PE_MAGIC = b"PE\0\0"

# DLL names are matched case-insensitively (Windows loader behaves the same).
TARGET_DLL_NAMES = (b"SHLWAPI.DLL", b"OLE32.DLL", b"EXPLORERFRAME.DLL")
WRAPPER_DLL_NAME = b"wrp64.dll"


class PEFormatError(ValueError):
    """Raised when the input is not a sane PE32+ AMD64 image."""


class _PE:
    """Minimal read-only view of a PE32+ image."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        n = len(data)
        if n < 0x40 or data[0:2] != DOS_MAGIC:
            raise PEFormatError("missing MZ header")
        (self.pe_off,) = struct.unpack_from("<I", data, 0x3C)
        if self.pe_off + 24 > n or data[self.pe_off:self.pe_off + 4] != PE_MAGIC:
            raise PEFormatError("missing PE\\0\\0 signature")
        coff = self.pe_off + 4
        (self.machine, self.num_sections, self.time_date_stamp,
         _ptr_sym, _num_sym, self.size_opt, self.characteristics) = \
            struct.unpack_from("<HHIIIHH", data, coff)
        if self.machine != IMAGE_FILE_MACHINE_AMD64:
            raise PEFormatError(f"not AMD64 (machine=0x{self.machine:04X})")
        self.opt_off = coff + 20
        (self.opt_magic,) = struct.unpack_from("<H", data, self.opt_off)
        if self.opt_magic != PE32PLUS_MAGIC:
            raise PEFormatError(f"not PE32+ (magic=0x{self.opt_magic:04X})")
        # CheckSum lives at offset 64 inside the optional header (PE32 and PE32+).
        self.checksum_off = self.opt_off + 64
        self.size_of_image, self.size_of_headers = struct.unpack_from(
            "<II", data, self.opt_off + 56)
        self.num_dirs_off = self.opt_off + 108
        (self.num_dirs,) = struct.unpack_from("<I", data, self.num_dirs_off)
        if self.num_dirs > 16:
            # More than the 16 standard entries: accept, we only read/write 2.
            pass
        self.dirs_off = self.num_dirs_off + 4
        self.sections = []
        sec_off = self.opt_off + self.size_opt
        for i in range(self.num_sections):
            base = sec_off + 40 * i
            if base + 40 > n:
                raise PEFormatError("section table truncated")
            (name, vsize, vaddr, raw_size, raw_off) = struct.unpack_from(
                "<8sIIII", data, base)
            self.sections.append({
                "name": name.rstrip(b"\0").decode("ascii", "replace"),
                "vsize": vsize, "vaddr": vaddr,
                "raw_size": raw_size, "raw_off": raw_off,
            })

    # -- data directories --------------------------------------------------
    def directory(self, index: int) -> tuple[int, int]:
        """Return (rva, size) of a data directory entry, (0, 0) if absent."""
        if index >= self.num_dirs:
            return (0, 0)
        rva, size = struct.unpack_from("<II", self.data, self.dirs_off + 8 * index)
        return (rva, size)

    def directory_field_offset(self, index: int) -> int:
        if index >= self.num_dirs:
            raise PEFormatError("directory index not present")
        return self.dirs_off + 8 * index

    # -- RVA translation ---------------------------------------------------
    def rva_to_off(self, rva: int) -> int:
        for s in self.sections:
            span = max(s["vsize"], s["raw_size"])
            if s["vaddr"] <= rva < s["vaddr"] + span:
                off = s["raw_off"] + (rva - s["vaddr"])
                if off >= len(self.data):
                    raise PEFormatError("RVA points past end of file")
                return off
        # Some directories live in the headers.
        if rva < self.size_of_headers and rva < len(self.data):
            return rva
        raise PEFormatError(f"RVA 0x{rva:08X} not covered by any section")

    def read_cstr(self, off: int, limit: int = 260) -> bytes:
        end = self.data.find(b"\0", off)
        if end == -1 or end - off > limit:
            raise PEFormatError(f"unterminated string at file offset 0x{off:X}")
        return self.data[off:end]

    # -- imports -----------------------------------------------------------
    def import_descriptors(self):
        """Yield (desc_off, name_rva, first_thunk) for each import descriptor."""
        rva, size = self.directory(IMAGE_DIRECTORY_ENTRY_IMPORT)
        if rva == 0:
            return
        off = self.rva_to_off(rva)
        # The import directory is an array of 20-byte descriptors, NUL terminated.
        while True:
            fields = struct.unpack_from("<IIIII", self.data, off)
            if all(f == 0 for f in fields):
                break
            yield off, fields[3], fields[4]
            off += 20
            if off > self.rva_to_off(rva) + size + 20 * 4:
                raise PEFormatError("import directory not NUL terminated")


# ---------------------------------------------------------------------------
# Deterministic PE checksum (matches ImageHlp/MapFileAndCheckSum semantics)
# ---------------------------------------------------------------------------

def pe_checksum(buf: bytearray, checksum_off: int) -> int:
    total = len(buf)
    s = 0
    i = 0
    while i + 2 <= total:
        if i == checksum_off:
            word = 0  # the CheckSum field itself is skipped
        else:
            word = buf[i] | (buf[i + 1] << 8)
        s = (s + word) & 0xFFFFFFFF
        s = (s & 0xFFFF) + (s >> 16)
        i += 2
    if total & 1:  # odd file size -> pad one zero byte
        if i == checksum_off:
            word = 0
        else:
            word = buf[total - 1]
        s = (s + word) & 0xFFFFFFFF
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    s = s + total
    return s & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# The patch
# ---------------------------------------------------------------------------

def patch_imports(data: bytes) -> tuple[bytes, list[str]]:
    """
    Return (patched_bytes, actions).

    actions is a human-readable log list, empty means "already patched /
    nothing to do" (idempotency: patch_imports(patch_imports(x)[0]) == x itself).
    """
    pe = _PE(data)
    buf = bytearray(data)  # byte-identical copy we edit
    actions: list[str] = []

    # 1) Rewrite the Name strings of the import descriptors we target.
    for desc_off, name_rva, _first_thunk in pe.import_descriptors():
        name_off = pe.rva_to_off(name_rva)
        name = pe.read_cstr(name_off)
        if name.upper() in TARGET_DLL_NAMES:
            replacement = WRAPPER_DLL_NAME + b"\0"
            # New name must fit in the old allocation; pad deterministically
            # with zeros up to the old length (keeps the file size stable).
            old_area = name + b"\0"
            if len(replacement) > len(old_area):
                raise PEFormatError(
                    f"replacement name longer than slot for {name!r}")
            pad = old_area[len(replacement):]
            buf[name_off:name_off + len(old_area)] = replacement + b"\0" * len(pad)
            actions.append(
                f"import {name.decode('ascii')} -> wrp64.dll "
                f"(desc@0x{desc_off:X}, name@0x{name_off:X})")
    # NOTE: "OLE32.DLL" and "wrp64.dll" are the same length; SHLWAPI.DLL and
    # EXPLORERFRAME.DLL are longer, the padding keeps the layout byte-stable.

    # 2) Clear the bound-import directory (if present). Its name strings are
    #    stored separately; zeroing the directory entry makes the loader
    #    resolve imports fresh, which is exactly the documented semantics.
    bnd_rva, bnd_size = pe.directory(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT)
    if bnd_rva or bnd_size:
        field = pe.directory_field_offset(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT)
        buf[field:field + 8] = b"\0" * 8
        actions.append("bound import directory cleared")

    # 3) Recompute the header CheckSum field (PE32+ offset 64 in opt header).
    #    Deterministic; matches MapFileAndCheckSum semantics.
    old_checksum = struct.unpack_from("<I", buf, pe.checksum_off)[0]
    struct.pack_into("<I", buf, pe.checksum_off, 0)
    new_checksum = pe_checksum(buf, pe.checksum_off)
    struct.pack_into("<I", buf, pe.checksum_off, new_checksum)
    if new_checksum != old_checksum:
        actions.append(
            f"PE CheckSum 0x{old_checksum:08X} -> 0x{new_checksum:08X}")

    return bytes(buf), actions


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print("usage: patch_imports.py <in.exe> [out.exe]", file=sys.stderr)
        return 2
    src = argv[1]
    dst = argv[2] if len(argv) == 3 else argv[1]
    with open(src, "rb") as f:
        original = f.read()
    patched, actions = patch_imports(original)
    print(f"SHA-256 before: {sha256_hex(original)}")
    for a in actions:
        print(f"  {a}")
    if not actions:
        print("  nothing to do (already patched or no target imports)")
    if patched != original:
        with open(dst, "wb") as f:
            f.write(patched)
        print(f"written: {dst}")
    print(f"SHA-256 after:  {sha256_hex(patched)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
