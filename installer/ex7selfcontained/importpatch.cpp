// importpatch.cpp — see importpatch.h.
// BUILD STATUS: syntax-reviewed only (sandbox has no Windows toolchain).
// The algorithm is a 1:1 port of tools/patch_imports.py, which is covered by
// tests/test_patch_imports.py; the C++ port must produce the same bytes on
// the same input (determinism harness on the user's machine:
//   ex7selfcontained.exe --selftest-importpatch <file> ).
#include "importpatch.h"
#include "config.h"

#include <cstring>

namespace ex7 {

namespace {

constexpr uint32_t kDirImport = 1;
constexpr uint32_t kDirBoundImport = 11;

struct PeView {
    const std::vector<uint8_t>& d;
    uint32_t peOff = 0, numSections = 0, optOff = 0, sizeOpt = 0;
    uint32_t dirsOff = 0, numDirs = 0, checksumOff = 0, sizeOfHeaders = 0;

    struct Section { uint32_t vsize, vaddr, rawSize, rawOff; };
    std::vector<Section> sections;

    bool in(const std::vector<uint8_t>& b, size_t off, size_t n) const {
        return off <= b.size() && n <= b.size() - off;
    }
    uint32_t u32(size_t off) const {
        uint32_t v; memcpy(&v, d.data() + off, 4); return v;
    }
    uint16_t u16(size_t off) const {
        uint16_t v; memcpy(&v, d.data() + off, 2); return v;
    }

    bool parse(std::wstring& err) {
        const size_t n = d.size();
        if (n < 0x40 || memcmp(d.data(), "MZ", 2) != 0) {
            err = L"no MZ"; return false;
        }
        peOff = u32(0x3C);
        if (!in(d, peOff, 24) || memcmp(d.data() + peOff, "PE\0\0", 4) != 0) {
            err = L"no PE"; return false;
        }
        if (u16(peOff + 4) != 0x8664) { err = L"not AMD64"; return false; }
        numSections = u16(peOff + 6);
        sizeOpt = u16(peOff + 20);
        optOff = peOff + 24;
        if (u16(optOff) != 0x20B) { err = L"not PE32+"; return false; }
        checksumOff = optOff + 64;
        sizeOfHeaders = u32(optOff + 60);
        numDirs = u32(optOff + 108);
        dirsOff = optOff + 112;
        size_t secOff = optOff + sizeOpt;
        for (uint32_t i = 0; i < numSections; ++i, secOff += 40) {
            if (!in(d, secOff, 40)) { err = L"section table truncated"; return false; }
            Section s{ u32(secOff + 8), u32(secOff + 12),
                       u32(secOff + 16), u32(secOff + 20) };
            sections.push_back(s);
        }
        return true;
    }

    bool directory(uint32_t idx, uint32_t& rva, uint32_t& size) const {
        rva = size = 0;
        if (idx >= numDirs) return false;
        rva = u32(dirsOff + 8 * idx);
        size = u32(dirsOff + 8 * idx + 4);
        return true;
    }
    size_t directoryFieldOffset(uint32_t idx) const { return dirsOff + 8 * idx; }

    bool rvaToOff(uint32_t rva, size_t& off) const {
        for (const auto& s : sections) {
            uint64_t span = s.vsize > s.rawSize ? s.vsize : s.rawSize;
            if (s.vaddr <= rva && (uint64_t)rva < (uint64_t)s.vaddr + span) {
                off = (size_t)s.rawOff + (rva - s.vaddr);
                if (off >= d.size()) return false;
                return true;
            }
        }
        if (rva < sizeOfHeaders && rva < d.size()) { off = rva; return true; }
        return false;
    }

    bool readCStr(size_t off, std::string& s, size_t limit = 260) const {
        size_t end = off;
        while (end < d.size() && d[end] != 0 && end - off < limit) ++end;
        if (end >= d.size() || d[end] != 0) return false;
        s.assign(reinterpret_cast<const char*>(d.data() + off), end - off);
        return true;
    }
};

bool isTargetName(const std::string& n) {
    char up[32]{};
    if (n.size() >= sizeof(up)) return false;
    for (size_t i = 0; i < n.size(); ++i)
        up[i] = (char)toupper((unsigned char)n[i]);
    for (const char* t : cfg::kPatchTargets)
        if (strcmp(up, t) == 0) return true;
    return false;
}

} // namespace

uint32_t ComputePeChecksum(const std::vector<uint8_t>& image,
                           size_t checksumFieldOffset) {
    uint64_t sum = 0;
    auto add = [&](uint16_t w) {
        sum += w;
        sum = (sum & 0xFFFF) + (sum >> 16);
    };
    const size_t total = image.size();
    size_t i = 0;
    for (; i + 2 <= total; i += 2) {
        if (i == checksumFieldOffset) continue;  // field itself is skipped
        add((uint16_t)(image[i] | (image[i + 1] << 8)));
    }
    if (total & 1) {
        if (i != checksumFieldOffset) add(image[total - 1]);
    }
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += total;
    return (uint32_t)(sum & 0xFFFFFFFF);
}

ImportPatchResult PatchImportsInPlace(std::vector<uint8_t>& image) {
    ImportPatchResult r;
    PeView pe{ image };
    if (!pe.parse(r.error)) return r;

    // sanity: this port must only ever run on the hash-verified file
    uint32_t impRva = 0, impSize = 0;
    if (!pe.directory(kDirImport, impRva, impSize) || impRva == 0) {
        r.error = L"no import directory";
        return r;
    }
    size_t descOff = 0;
    if (!pe.rvaToOff(impRva, descOff)) {
        r.error = L"import directory RVA unmapped";
        return r;
    }
    size_t impEnd = descOff + impSize + 20 * 4;  // slack like the Python port

    const std::string replacement = std::string(cfg::kWrapperName) + '\0';

    for (size_t off = descOff; off + 20 <= impEnd && off + 20 <= image.size();
         off += 20) {
        uint32_t f[5];
        memcpy(f, image.data() + off, 20);
        if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0 && f[4] == 0)
            break;  // NUL terminator reached
        size_t nameOff = 0;
        if (!pe.rvaToOff(f[3], nameOff)) continue;
        std::string name;
        if (!pe.readCStr(nameOff, name)) continue;
        if (!isTargetName(name)) continue;

        const size_t slot = name.size() + 1;      // old name incl. NUL
        if (replacement.size() > slot) {
            r.error = L"replacement name longer than slot";
            return r;
        }
        memcpy(image.data() + nameOff, replacement.data(),
               replacement.size());
        memset(image.data() + nameOff + replacement.size(), 0,
               slot - replacement.size());        // deterministic padding
        r.actions.push_back("import " + name + " -> " + cfg::kWrapperName);
        ++r.namesPatched;
    }

    uint32_t bndRva = 0, bndSize = 0;
    pe.directory(kDirBoundImport, bndRva, bndSize);
    if (bndRva || bndSize) {
        size_t field = pe.directoryFieldOffset(kDirBoundImport);
        memset(image.data() + field, 0, 8);
        r.actions.push_back("bound import directory cleared");
        r.boundImportCleared = true;
    }

    uint32_t oldSum = pe.u32(pe.checksumOff);
    // The whole 4-byte CheckSum field is EXCLUDED from the checksum: zero it
    // BEFORE computing (bug found by the real-file CI comparison: the high
    // word of a previous non-zero checksum must not leak into the new one).
    memset(image.data() + pe.checksumOff, 0, 4);
    uint32_t newSum = ComputePeChecksum(image, pe.checksumOff);
    if (newSum != oldSum) {
        memcpy(image.data() + pe.checksumOff, &newSum, 4);
        r.actions.push_back("PE CheckSum updated");
    } else {
        memcpy(image.data() + pe.checksumOff, &newSum, 4);  // keep stored
    }

    r.ok = true;
    return r;
}

} // namespace ex7
