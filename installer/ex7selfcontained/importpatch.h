// importpatch.h — byte-level port of tools/patch_imports.py.
// Rewrites SHLWAPI.DLL / OLE32.DLL / EXPLORERFRAME.DLL -> "wrp64.dll" in the
// import directory of an in-memory PE image. Deterministic and idempotent,
// byte-verifiable against the Python reference (tests/test_patch_imports.py);
// see docs/PIANO_INSTALLAZIONE_SELFCONTAINED.md, task 2.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ex7 {

struct ImportPatchResult {
    bool ok = false;
    std::wstring error;                 // set when ok == false
    std::vector<std::string> actions;   // human-readable log lines
    unsigned int namesPatched = 0;      // count of target DLL names rewritten
    bool boundImportCleared = false;
};

// `image` is modified in place. On ok == false the buffer content is
// unspecified but never longer/shorter than before.
ImportPatchResult PatchImportsInPlace(std::vector<uint8_t>& image);

// Also recomputes the optional-header CheckSum field (deterministic,
// MapFileAndCheckSum-compatible) — used by the patch and by tests.
uint32_t ComputePeChecksum(const std::vector<uint8_t>& image,
                           size_t checksumFieldOffset);

} // namespace ex7
