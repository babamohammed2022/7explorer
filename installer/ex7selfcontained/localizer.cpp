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
    if (!opt.fullExplorerTransplanted && !opt.forceAllowPartial) {
        // Safety gate: without a successful transplant the copy would have
        // strings but NO menu/dialog resources at all; neutralization must
        // wait (or be forced) or dialogs/menus would break in the shell.
        error = L"coverage gate: menus/dialogs not transplanted; "
                L"neutralization refused (place a verified explorer.exe.mui "
                L"next to the installer or use --allow-partial-localization)";
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

// ================= MUI transplant (menus / dialogs / strings) ===============
// Mirrors tools/analyze_mui.py EXACTLY (same walking order => same level/index
// and occurrence keys as localization/constraints/explorer.exe.constraints.json
// and localization/catalog/*.json). Everything but the replaced text is copied
// verbatim from the reference template bytes supplied by the user at install
// time (no Microsoft bytes ever ship in this repository).

#include "config.h"
#include "winhash.h"

#include <set>

namespace ex7 {

// ---- tiny LE cursor --------------------------------------------------------
struct Cur {
    const unsigned char* p;
    size_t n, off;
    bool u8(unsigned char& v) { if (off + 1 > n) return false; v = p[off++]; return true; }
    bool u16(unsigned short& v) {
        if (off + 2 > n) return false;
        v = (unsigned short)(p[off] | (p[off + 1] << 8)); off += 2; return true;
    }
    bool u32(unsigned& v) {
        if (off + 4 > n) return false;
        v = p[off] | (p[off + 1] << 8) | ((unsigned)p[off + 2] << 16) | ((unsigned)p[off + 3] << 24);
        off += 4; return true;
    }
};

static void Put16(std::vector<unsigned char>& o, unsigned short v) {
    o.push_back((unsigned char)(v & 0xFF)); o.push_back((unsigned char)(v >> 8));
}
static void Put32(std::vector<unsigned char>& o, unsigned v) {
    o.push_back((unsigned char)(v & 0xFF));
    o.push_back((unsigned char)((v >> 8) & 0xFF));
    o.push_back((unsigned char)((v >> 16) & 0xFF));
    o.push_back((unsigned char)((v >> 24) & 0xFF));
}
static void PutWStrZ(std::vector<unsigned char>& o, const std::wstring& s) {
    for (wchar_t c : s) Put16(o, (unsigned short)c);
    Put16(o, 0);
}
static void Align4(std::vector<unsigned char>& o) {
    while (o.size() & 3) o.push_back(0);
}

// UTF-16LE NUL-terminated string at cursor (parity-safe, mirrors u16_cstr_end).
static bool ReadWStrZ(Cur& c, std::wstring& out, size_t& first, size_t& last) {
    first = c.off;
    size_t i = c.off;
    for (;;) {
        if (i + 1 >= c.n) return false;
        if (c.p[i] == 0 && c.p[i + 1] == 0) {
            size_t end = ((i - c.off) & 1) ? i + 1 : i;
            out.assign((const wchar_t*)(c.p + c.off), (end - c.off) / 2);
            last = end;
            c.off = end + 2;
            return true;
        }
        i += 2;
    }
}

// ---- text maps -------------------------------------------------------------
typedef std::map<std::pair<unsigned, unsigned>, std::wstring> KeyedTexts;

static KeyedTexts MenuTexts(const Ex7LangMenu& m) {
    KeyedTexts t;
    for (unsigned i = 0; i < m.itemCount; ++i)
        t[std::make_pair(m.items[i].level, m.items[i].index)] = m.items[i].text;
    return t;
}

// ---- MENU rebuild ----------------------------------------------------------
// Returns false (leave original) on any parse surprise: never invent layout.
static bool WalkMenuClassicLevel(Cur& c, std::vector<unsigned char>& out,
                                 unsigned level, unsigned& nextLevel,
                                 const KeyedTexts& texts) {
    for (unsigned idx = 0;; ++idx) {
        unsigned short flags;
        if (!c.u16(flags)) return false;
        std::wstring text;
        size_t tf, tl;
        if (!ReadWStrZ(c, text, tf, tl)) return false;
        auto it = texts.find(std::make_pair(level, idx));
        const std::wstring& chosen = (it == texts.end()) ? text : it->second;
        bool popup = (flags & 0x10) != 0;
        unsigned myLevelForChild = 0;
        if (popup) myLevelForChild = nextLevel++;
        Put16(out, flags);
        PutWStrZ(out, chosen);
        if (!popup) {
            unsigned short cmd;
            if (!c.u16(cmd)) return false;
            Put16(out, cmd);
        }
        if (popup) {
            if (!WalkMenuClassicLevel(c, out, myLevelForChild, nextLevel,
                                      texts))
                return false;
        }
        if (flags & 0x80) break;
    }
    return true;
}

static bool WalkMenuExLevel(Cur& c, std::vector<unsigned char>& out,
                            unsigned level, unsigned& nextLevel,
                            const KeyedTexts& texts) {
    for (unsigned idx = 0;; ++idx) {
        unsigned dwType, dwState, uId;
        unsigned char bResInfo;
        if (!c.u32(dwType) || !c.u32(dwState) || !c.u32(uId) ||
            !c.u8(bResInfo))
            return false;
        std::wstring text;
        size_t tf, tl;
        if (!ReadWStrZ(c, text, tf, tl)) return false;
        c.off = (c.off + 3) & ~size_t(3);
        bool popup = (bResInfo & 0x01) != 0;
        unsigned helpId = 0;
        if (popup && !c.u32(helpId)) return false;
        auto it = texts.find(std::make_pair(level, idx));
        const std::wstring& chosen = (it == texts.end()) ? text : it->second;
        unsigned myLevelForChild = 0;
        if (popup) myLevelForChild = nextLevel++;
        Put32(out, dwType); Put32(out, dwState); Put32(out, uId);
        out.push_back(bResInfo);
        PutWStrZ(out, chosen);
        Align4(out);
        if (popup) {
            Put32(out, helpId);
            if (!WalkMenuExLevel(c, out, myLevelForChild, nextLevel, texts))
                return false;
        }
        if (bResInfo & 0x80) break;
    }
    return true;
}

bool RebuildMenuPayload(const std::vector<unsigned char>& src,
                        const Ex7LangMenu& ref,
                        std::vector<unsigned char>& out) {
    if (src.size() < 4) return false;
    KeyedTexts texts = MenuTexts(ref);
    unsigned short version, header;
    Cur c{src.data(), src.size(), 0};
    c.u16(version); c.u16(header);
    if (version == 0) {
        out.assign(src.begin(), src.begin() + 4);
        Cur body{src.data(), src.size(), 4};
        unsigned nextLevel = 1;
        return WalkMenuClassicLevel(body, out, 0, nextLevel, texts);
    }
    if (version == 1 && header == 4) {
        out.assign(src.begin(), src.begin() + 8);
        Cur body{src.data(), src.size(), 8};
        unsigned nextLevel = 1;
        return WalkMenuExLevel(body, out, 0, nextLevel, texts);
    }
    return false;
}

// ---- DIALOG(EX) rebuild ----------------------------------------------------
// sz/ordinal: 0x0000 -> null; 0xFFFF oooo -> ordinal; else UTF-16 string.
// Returns kind: 0 null, 1 ordinal, 2 string. Cursor advances past the field.
static int ReadSzOrOrd(Cur& c, std::wstring& s, unsigned short& ord) {
    unsigned short w;
    if (!c.u16(w)) return -1;
    if (w == 0x0000) return 0;
    if (w == 0xFFFF) { if (!c.u16(ord)) return -1; return 1; }
    size_t first = c.off - 2;
    size_t i = first;
    for (;;) {
        if (i + 1 >= c.n) return -1;
        if (c.p[i] == 0 && c.p[i + 1] == 0) {
            size_t end = ((i - first) & 1) ? i + 1 : first;  // first is even
            end = ((i - first) & 1) ? i + 1 : i;
            s.assign((const wchar_t*)(c.p + first), (end - first) / 2);
            c.off = end + 2;
            return 2;
        }
        i += 2;
    }
}
static void PutSzOrOrd(std::vector<unsigned char>& o, int kind,
                       const std::wstring& s, unsigned short ord) {
    if (kind == 0) Put16(o, 0);
    else if (kind == 1) { Put16(o, 0xFFFF); Put16(o, ord); }
    else PutWStrZ(o, s);
}
static void CopyRaw(Cur& c, std::vector<unsigned char>& o, size_t nBytes) {
    o.insert(o.end(), c.p + c.off, c.p + c.off + nBytes);
    c.off += nBytes;
}

bool RebuildDialogPayload(const std::vector<unsigned char>& src,
                          const Ex7LangDialog& ref,
                          std::vector<unsigned char>& out) {
    if (src.size() < 4) return false;
    KeyedTexts texts;
    for (unsigned i = 0; i < ref.ctlCount; ++i)
        texts[std::make_pair(ref.ctls[i].id, ref.ctls[i].occurrence)] =
            ref.ctls[i].text;

    Cur c{src.data(), src.size(), 0};
    unsigned short sig, sig2;
    c.u16(sig); c.u16(sig2);
    bool ex = (sig == 0xFFFF && sig2 == 0xFFFF);
    out.clear();
    Put16(out, sig); Put16(out, sig2);

    unsigned style = 0;
    unsigned short cItems = 0;
    if (ex) {
        unsigned help, exs;
        if (!c.u32(help) || !c.u32(exs) || !c.u32(style) || !c.u16(cItems))
            return false;
        Put32(out, help); Put32(out, exs); Put32(out, style);
        Put16(out, cItems);
    } else {
        unsigned exs;
        if (!c.u32(style) || !c.u32(exs) || !c.u16(cItems)) return false;
        Put32(out, style); Put32(out, exs); Put16(out, cItems);
    }
    // x, y, cx, cy
    for (int i = 0; i < 4; ++i) {
        unsigned short v; if (!c.u16(v)) return false;
        Put16(out, v);
    }
    // menu, class: copy verbatim fields (they may be null/ordinal/string)
    for (int f = 0; f < 2; ++f) {
        std::wstring s; unsigned short ord = 0;
        int kind = ReadSzOrOrd(c, s, ord);
        if (kind < 0) return false;
        PutSzOrOrd(out, kind, s, ord);
    }
    // title: replace when the catalog defines one
    {
        std::wstring s; unsigned short ord = 0;
        int kind = ReadSzOrOrd(c, s, ord);
        if (kind < 0) return false;
        if (ref.title && kind == 2) PutWStrZ(out, ref.title);
        else PutSzOrOrd(out, kind, s, ord);
    }
    if (style & 0x40) {  // DS_SETFONT / DS_SHELLFONT: copy font block verbatim
        if (ex) {
            unsigned short pts, weight; unsigned char it, ch;
            if (!c.u16(pts) || !c.u16(weight) || !c.u8(it) || !c.u8(ch))
                return false;
            Put16(out, pts); Put16(out, weight); out.push_back(it);
            out.push_back(ch);
        } else {
            unsigned short pts; if (!c.u16(pts)) return false;
            Put16(out, pts);
        }
        std::wstring face; unsigned short ord = 0;
        int kind = ReadSzOrOrd(c, face, ord);
        if (kind < 0) return false;
        PutSzOrOrd(out, kind, face, ord);
    }

    std::map<unsigned, unsigned> seen;  // occurrence per control id (text only)
    for (unsigned ci = 0; ci < cItems; ++ci) {
        Align4(out);
        c.off = (c.off + 3) & ~size_t(3);
        unsigned id = 0;
        if (ex) {
            unsigned help, exs, st;
            if (!c.u32(help) || !c.u32(exs) || !c.u32(st)) return false;
            Put32(out, help); Put32(out, exs); Put32(out, st);
            for (int i = 0; i < 4; ++i) {
                unsigned short v; if (!c.u16(v)) return false; Put16(out, v);
            }
            if (!c.u32(id)) return false;
            Put32(out, id);
        } else {
            unsigned st, exs;
            if (!c.u32(st) || !c.u32(exs)) return false;
            Put32(out, st); Put32(out, exs);
            for (int i = 0; i < 4; ++i) {
                unsigned short v; if (!c.u16(v)) return false; Put16(out, v);
            }
            unsigned short idw; if (!c.u16(idw)) return false;
            id = idw; Put16(out, idw);
        }
        {
            std::wstring s2; unsigned short ord = 0;
            int kind = ReadSzOrOrd(c, s2, ord);  // window class
            if (kind < 0) return false;
            PutSzOrOrd(out, kind, s2, ord);
        }
        {
            std::wstring s2; unsigned short ord = 0;
            int kind = ReadSzOrOrd(c, s2, ord);  // control text
            if (kind < 0) return false;
            bool replacedCtl = false;
            if (kind == 2 && !s2.empty()) {
                unsigned occ = ++seen[id];
                auto it = texts.find(std::make_pair(id, occ));
                if (it != texts.end()) {
                    PutWStrZ(out, it->second);
                    replacedCtl = true;
                }
            }
            if (!replacedCtl) PutSzOrOrd(out, kind, s2, ord);
        }
        unsigned short extraN;
        if (!c.u16(extraN)) return false;
        Put16(out, extraN);
        if (extraN) {
            if (c.off + extraN > c.n) return false;
            CopyRaw(c, out, extraN);
        }
    }
    return true;
}

} // namespace ex7

// ================= reference .mui identity + transplant wiring ==============
namespace ex7 {

// Minimal PE identity check (machine + TimeDateStamp + exact size) plus the
// pinned SHA-256 allow-list. Same philosophy as the explorer.exe identity.
bool VerifyReferenceMui(const std::wstring& muiPath, std::wstring& error) {
    HANDLE h = OpenForReadShared(muiPath);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"cannot open reference .mui";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart != (LONGLONG)cfg::kMuiSize) {
        CloseHandle(h);
        error = L"reference .mui: unexpected file size";
        return false;
    }
    unsigned char hdr[4096];
    DWORD got = 0;
    LARGE_INTEGER zero{};
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    if (!ReadFile(h, hdr, sizeof(hdr), &got, nullptr) || got < 1024 ||
        hdr[0] != 'M' || hdr[1] != 'Z') {
        CloseHandle(h);
        error = L"reference .mui: not a PE file";
        return false;
    }
    unsigned pe = hdr[0x3C] | (hdr[0x3D] << 8) | ((unsigned)hdr[0x3E] << 16) |
                  ((unsigned)hdr[0x3F] << 24);
    bool okpe = false;
    if (pe + 24 <= sizeof(hdr) && hdr[pe] == 'P' && hdr[pe + 1] == 'E' &&
        hdr[pe + 2] == 0 && hdr[pe + 3] == 0) {
        unsigned machine = hdr[pe + 4] | (hdr[pe + 5] << 8);
        unsigned tds = hdr[pe + 8] | (hdr[pe + 9] << 8) |
                       ((unsigned)hdr[pe + 10] << 16) |
                       ((unsigned)hdr[pe + 11] << 24);
        okpe = (machine == 0x8664) && (tds == cfg::kMuiTimeDateStamp);
    }
    if (!okpe) {
        CloseHandle(h);
        error = L"reference .mui: machine/TimeDateStamp mismatch";
        return false;
    }
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    std::wstring hex = Sha256HexOfHandle(h);
    CloseHandle(h);
    bool hashOk = false;
    wchar_t variant = L'?';
    for (unsigned i = 0; i < cfg::kMuiAcceptedSha256Count; ++i)
        if (Sha256HexEqualsCI(hex, cfg::kMuiAcceptedSha256[i])) {
            hashOk = true;
            variant = (wchar_t)(L'A' + i);
            break;
        }
    if (!hashOk) {
        wchar_t buf[160];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"reference .mui SHA256 mismatch: got %s (allow-list of "
                     L"%u documented variants)", hex.c_str(),
                     cfg::kMuiAcceptedSha256Count);
        error = buf;
        return false;
    }
    Log(L"reference .mui identity ok (allow-list variant %c)", variant);
    return true;
}

// Reads one resource payload out of the reference .mui (any language).
static bool GetResBytes(HMODULE mod, const wchar_t* type, unsigned id,
                        WORD lang, std::vector<unsigned char>& out) {
    HRSRC r = FindResourceExW(mod, type, MAKEINTRESOURCEW(id), lang);
    if (!r) {
        // fall back to whatever language exists for that name
        r = FindResourceW(mod, MAKEINTRESOURCEW(id), type);
    }
    if (!r) return false;
    HGLOBAL g = LoadResource(mod, r);
    DWORD sz = SizeofResource(mod, r);
    if (!g || !sz) return false;
    const unsigned char* p =
        static_cast<const unsigned char*>(LockResource(g));
    out.assign(p, p + sz);
    return true;
}

bool TransplantMuiResources(const std::wstring& muiPath,
                            const std::wstring& exePath,
                            std::wstring& error) {
    if (!VerifyReferenceMui(muiPath, error)) return false;

    HMODULE mod = LoadLibraryExW(muiPath.c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE |
                                 LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
        error = L"cannot map reference .mui";
        return false;
    }

    HANDLE h = BeginUpdateResourceW(exePath.c_str(), FALSE);
    if (!h) {
        FreeLibrary(mod);
        error = L"BeginUpdateResource failed";
        return false;
    }

    const WORD kRefLang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    bool ok = true;
    auto fail = [&](const wchar_t* what, unsigned id) {
        wchar_t buf[160];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"transplant failed at %s %u", what, id);
        error = buf;
        ok = false;
    };

    unsigned injected = 0;
    for (unsigned t = 0; t < g_ex7LangTableCount && ok; ++t) {
        const Ex7LangTable& tab = g_ex7LangTables[t];
        if (!tab.hasFullExplorer) continue;  // strings only for these langs

        // -- menus ------------------------------------------------------------
        for (unsigned m = 0; m < tab.menuCount && ok; ++m) {
            const Ex7LangMenu& ref = tab.menus[m];
            std::vector<unsigned char> orig, out;
            if (!GetResBytes(mod, RT_MENU, ref.resId, kRefLang, orig)) {
                fail(L"menu-read", ref.resId);
                break;
            }
            if (!RebuildMenuPayload(orig, ref, out)) {
                fail(L"menu-rebuild", ref.resId);
                break;
            }
            if (!UpdateResourceW(h, RT_MENU, MAKEINTRESOURCEW(ref.resId),
                                 (WORD)tab.lcid, out.data(),
                                 (DWORD)out.size())) {
                fail(L"menu-write", ref.resId);
                break;
            }
        }
        // -- dialogs ------------------------------------------------------------
        for (unsigned d = 0; d < tab.dialogCount && ok; ++d) {
            const Ex7LangDialog& ref = tab.dialogs[d];
            std::vector<unsigned char> orig, out;
            if (!GetResBytes(mod, RT_DIALOG, ref.resId, kRefLang, orig)) {
                fail(L"dialog-read", ref.resId);
                break;
            }
            if (!RebuildDialogPayload(orig, ref, out)) {
                fail(L"dialog-rebuild", ref.resId);
                break;
            }
            if (!UpdateResourceW(h, RT_DIALOG, MAKEINTRESOURCEW(ref.resId),
                                 (WORD)tab.lcid, out.data(),
                                 (DWORD)out.size())) {
                fail(L"dialog-write", ref.resId);
                break;
            }
        }
        // -- accelerators: id 251 has no text; copy verbatim per language -----
        {
            std::vector<unsigned char> acc;
            if (GetResBytes(mod, RT_ACCELERATOR, 251, kRefLang, acc)) {
                if (!UpdateResourceW(h, RT_ACCELERATOR, MAKEINTRESOURCEW(251),
                                     (WORD)tab.lcid, acc.data(),
                                     (DWORD)acc.size())) {
                    fail(L"accel-write", 251);
                    break;
                }
            }
        }
        ++injected;
        Log(L"transplanted menus/dialogs/accelerators for %ls (lcid 0x%04X)",
            tab.muiName, tab.lcid);
    }

    if (!EndUpdateResourceW(h, !ok /* discard on failure */)) {
        error = L"EndUpdateResource failed";
        ok = false;
    }
    FreeLibrary(mod);
    if (ok && injected == 0)
        error = L"no catalog language covers explorer.exe.mui fully yet "
                L"(menus/dialogs not transplanted)";
    return ok && injected > 0;
}

} // namespace ex7
