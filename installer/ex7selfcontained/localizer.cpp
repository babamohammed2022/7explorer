// localizer.cpp — see localizer.h.
// BUILD STATUS: syntax-reviewed only (sandbox has no Windows toolchain).
#include "localizer.h"

#include <windows.h>

#include <cstring>
#include <map>

#include "lang_catalog.h"   // GENERATED from localization/catalog/*.json

namespace ex7 {

extern void (*g_log)(const wchar_t* fmt, ...);
static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[1024];
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(L"%s", buf);
}

std::vector<unsigned char> BuildStringBlockPayload(
    unsigned int blockId,
    const Ex7LangStringEntry* entries, unsigned int count) {
    const unsigned int base = (blockId - 1) * 16;
    std::map<unsigned int, const wchar_t*> byId;
    for (unsigned int i = 0; i < count; ++i)
        if (entries[i].id >= base + 1 && entries[i].id < base + 17)
            byId[entries[i].id] = entries[i].text;

    std::vector<unsigned char> out;
    auto put16 = [&](unsigned short v) {
        out.push_back((unsigned char)(v & 0xFF));
        out.push_back((unsigned char)(v >> 8));
    };
    for (unsigned int i = 0; i < 16; ++i) {
        auto it = byId.find(base + 1 + i);
        const wchar_t* s = (it == byId.end()) ? L"" : it->second;
        size_t len = wcslen(s);
        if (len > 0xFFFF) len = 0xFFFF;
        put16((unsigned short)len);
        for (size_t k = 0; k < len; ++k)
            put16((unsigned short)s[k]);
    }
    return out;
}

bool InjectCatalogStrings(const std::wstring& exePath, std::wstring& error) {
    // FALSE: keep every existing resource of the private copy; we only add /
    // replace the exact (RT_STRING, block, lcid) entries we write.
    HANDLE h = BeginUpdateResourceW(exePath.c_str(), FALSE);
    if (!h) {
        error = L"BeginUpdateResource failed";
        return false;
    }
    bool ok = true;
    for (unsigned int t = 0; t < g_ex7LangTableCount && ok; ++t) {
        const Ex7LangTable& tab = g_ex7LangTables[t];
        // group by 16-string block
        std::map<unsigned int, bool> blocks;
        for (unsigned int i = 0; i < tab.count; ++i)
            blocks[(tab.entries[i].id - 1) / 16 + 1] = true;
        for (const auto& kv : blocks) {
            unsigned int blockId = kv.first;
            std::vector<unsigned char> payload =
                BuildStringBlockPayload(blockId, tab.entries, tab.count);
            if (!UpdateResourceW(h, RT_STRING, MAKEINTRESOURCEW(blockId),
                                 (WORD)tab.lcid,
                                 (LPVOID)payload.data(),
                                 (DWORD)payload.size())) {
                error = L"UpdateResource failed for a string block";
                ok = false;
                break;
            }
        }
    }
    if (!EndUpdateResourceW(h, !ok /* discard on failure */)) {
        error = L"EndUpdateResource failed";
        return false;
    }
    if (ok)
        Log(L"injected catalog strings into %s", exePath.c_str());
    return ok;
}

// Reads the existing "MUI" resource bytes (so they can be re-added as "CUI").
static bool ReadMuiResourceBytes(const std::wstring& exePath,
                                 std::vector<unsigned char>& bytes,
                                 WORD& langOut) {
    HMODULE mod = LoadLibraryExW(exePath.c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE |
                                 LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) return false;
    bool ok = false;
    // The MUI resource type is the string "MUI", name id 1.
    HRSRC r = FindResourceExW(mod, L"MUI", MAKEINTRESOURCEW(1),
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    if (!r) {
        // find whatever language id Windows picked for it
        r = FindResourceW(mod, MAKEINTRESOURCEW(1), L"MUI");
    }
    if (r) {
        HGLOBAL g = LoadResource(mod, r);
        DWORD sz = SizeofResource(mod, r);
        if (g && sz) {
            const unsigned char* p =
                static_cast<const unsigned char*>(LockResource(g));
            bytes.assign(p, p + sz);
            ok = true;
        }
    }
    FreeLibrary(mod);
    langOut = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    return ok;
}

bool NeutralizeMuiResource(const std::wstring& exePath,
                           const LocalizeOptions& opt,
                           std::wstring& error) {
    if (!opt.forceAllowPartial) {
        // Coverage gate: without the explorer-exe constraints file we cannot
        // PROVE the embedded catalog covers every string the original .mui
        // served; refusing prevents a partially-localized shell.
        error = L"coverage gate: explorer.exe constraints not available; "
                L"neutralization refused (see PIANO docs)";
        return false;
    }
    Log(L"WARNING: allowPartial in effect — neutralizing MUI resource "
        L"without full coverage proof");

    std::vector<unsigned char> bytes;
    WORD lang = 0;
    if (!ReadMuiResourceBytes(exePath, bytes, lang) || bytes.empty()) {
        error = L"no MUI resource found (already neutralized?)";
        return false;
    }

    HANDLE h = BeginUpdateResourceW(exePath.c_str(), FALSE);
    if (!h) { error = L"BeginUpdateResource failed"; return false; }

    // delete the old "MUI"-typed resource, re-add identical bytes as "CUI"
    if (!UpdateResourceW(h, L"MUI", MAKEINTRESOURCEW(1), lang, nullptr, 0)) {
        EndUpdateResourceW(h, TRUE);
        error = L"failed to delete MUI resource";
        return false;
    }
    if (!UpdateResourceW(h, L"CUI", MAKEINTRESOURCEW(1), lang,
                         bytes.data(), (DWORD)bytes.size())) {
        EndUpdateResourceW(h, TRUE);
        error = L"failed to add CUI resource";
        return false;
    }
    if (!EndUpdateResourceW(h, FALSE)) {
        error = L"EndUpdateResource failed";
        return false;
    }
    Log(L"neutralized MUI resource in %s (MUI -> CUI)", exePath.c_str());
    return true;
}

} // namespace ex7
