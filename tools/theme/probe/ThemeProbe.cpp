// ThemeProbe.cpp — CI-only diagnostic: loads a .msstyles exactly the way
// the wrapper's ThemeManager does (uxtheme ordinal 7/92/16 internals) and
// prints every HRESULT.  Lets us learn, on clean Windows runners, how
// uxtheme treats an unsigned, self-authored theme WITHOUT touching any
// user machine.  Build: cl /MT /W4 ThemeProbe.cpp
#include <windows.h>
#include <uxtheme.h>
#include <stdio.h>
#pragma comment(lib, "uxtheme.lib")

// same layout as explorerwrapper/ThemeManager.h (upstream code, same repo)
struct UXTHEMEFILE
{
    char header[7]; // "thmfile"
    LPVOID sharableSectionView;
    HANDLE hSharableSection;
    LPVOID nsSectionView;
    HANDLE hNsSection;
    char end[3]; // "end"
};

typedef HRESULT(WINAPI *GetThemeDefaults_t)(LPCWSTR, LPWSTR, DWORD,
                                            LPWSTR, DWORD);
typedef HTHEME(WINAPI *OpenThemeDataFromFile_t)(UXTHEMEFILE*, HWND,
                                                LPCWSTR, DWORD);
typedef HRESULT(WINAPI *LoaderLoadTheme_t)(
    HANDLE, HINSTANCE, LPCWSTR, LPCWSTR, LPCWSTR,
    OUT HANDLE*, LPWSTR, int, OUT HANDLE*, LPWSTR, int,
    PVOID, OUT HANDLE*, int, int, BOOL);

// newer Win11 builds dropped the last arg — probe both, like upstream
typedef HRESULT(WINAPI *LoaderLoadTheme_t_win11)(
    HANDLE, HINSTANCE, LPCWSTR, LPCWSTR, LPCWSTR,
    OUT HANDLE*, LPWSTR, int, OUT HANDLE*, LPWSTR, int,
    PVOID, OUT HANDLE*, int, int);

static GetThemeDefaults_t pGetThemeDefaults;
static BOOL g_forceDefaults = FALSE;
static FARPROC pLoaderLoadTheme;
static OpenThemeDataFromFile_t pOpenThemeDataFromFile;

// ---- structural enumeration (names/ids ONLY, no content bytes) -----------
// Lists the resource type/name/lang triples of a PE (--enum mode). Used to
// learn the resource LAYOUT of themes (which custom types/names exist and
// their payload sizes), never to read their content.

static LPCWSTR g_enumPath;

static void PrintName(LPCWSTR label, LPCWSTR v)
{
    if (IS_INTRESOURCE(v))
        wprintf(L"      %s: #%lu\n", label, (unsigned long)(UINT_PTR)v);
    else
        wprintf(L"      %s: '%s'\n", label, v);
}

static BOOL CALLBACK EnumLangCB(HMODULE hModule, LPCWSTR lpType,
                                LPCWSTR lpName, WORD wLang, LONG_PTR lParam)
{
    HRSRC h = FindResourceExW(hModule, lpType, lpName, wLang);
    DWORD sz = h ? SizeofResource(hModule, h) : 0;
    wprintf(L"        lang 0x%04x  size %lu\n", (unsigned)wLang,
            (unsigned long)sz);
    return TRUE;
}

static BOOL CALLBACK EnumNameCB(HMODULE hModule, LPCWSTR lpType,
                                LPWSTR lpName, LONG_PTR lParam)
{
    PrintName(L"name", lpName);
    EnumResourceLanguagesW(hModule, lpType, lpName, EnumLangCB, 0);
    return TRUE;
}

static BOOL CALLBACK EnumTypeCB(HMODULE hModule, LPWSTR lpType,
                                LONG_PTR lParam)
{
    if (IS_INTRESOURCE(lpType))
        wprintf(L"   TYPE #%lu\n", (unsigned long)(UINT_PTR)lpType);
    else
        wprintf(L"   TYPE '%s'\n", lpType);
    EnumResourceNamesW(hModule, lpType, EnumNameCB, 0);
    return TRUE;
}

static void EnumResources(LPCWSTR path)
{
    wprintf(L"== enum %s\n", path);
    HMODULE m = LoadLibraryExW(path, NULL,
                               LOAD_LIBRARY_AS_DATAFILE |
                               LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        wprintf(L"   !! LoadLibraryEx failed (err %lu)\n", GetLastError());
        return;
    }
    EnumResourceTypesW(m, EnumTypeCB, 0);
    FreeLibrary(m);
}


static void PeHeader(LPCWSTR path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { wprintf(L"!! open err %lu\n",
        GetLastError()); return; }
    unsigned char hdr[512]; DWORD rd = 0;
    ReadFile(f, hdr, sizeof(hdr), &rd, NULL);
    CloseHandle(f);
    DWORD peOff = *(DWORD*)(hdr + 0x3C);
    WORD machine = *(WORD*)(hdr + peOff + 4);
    WORD chars = *(WORD*)(hdr + peOff + 4 + 18);
    WORD magic = *(WORD*)(hdr + peOff + 24);
    wprintf(L"== pehdr %s\n   machine=0x%04x (%s) optmagic=0x%04x (%s) "
            L"chars=0x%04x\n", path, machine,
            machine == 0x14c ? L"i386" : machine == 0x8664 ? L"AMD64"
            : L"other", magic, magic == 0x10b ? L"PE32"
            : magic == 0x20b ? L"PE32+" : L"?", chars);
}


// ---- carve experiment: remove resources from a copy of a real theme ----
static WCHAR g_keep[32][64]; static int g_nKeep;
static BOOL NameInKeep(LPCWSTR v)
{
    if (IS_INTRESOURCE(v)) {
        WCHAR buf[16]; swprintf(buf, 16, L"#%d", (int)(DWORD_PTR)v);
        for (int i = 0; i < g_nKeep; i++)
            if (!lstrcmpiW(g_keep[i], buf)) return TRUE;
        return FALSE;
    }
    for (int i = 0; i < g_nKeep; i++)
        if (!lstrcmpiW(g_keep[i], v)) return TRUE;
    return FALSE;
}
static HMODULE g_carveH;
struct CarveTypeCtx { LPCWSTR type; HMODULE h; };
static BOOL CALLBACK CarveLangW(HMODULE m, LPCWSTR t, LPCWSTR n, WORD lang,
                                LONG_PTR)
{
    if (!NameInKeep(t) && !NameInKeep(n)) {
        UpdateResourceW(g_carveH, t, n, lang, NULL, 0);
    }
    return TRUE;
}
static BOOL CALLBACK CarveNameW(HMODULE m, LPCWSTR t, LPWSTR n, LONG_PTR)
{
    EnumResourceLanguagesW(m, t, n, CarveLangW, 0);
    return TRUE;
}
static BOOL CALLBACK CarveTypeW(HMODULE m, LPWSTR t, LONG_PTR)
{
    EnumResourceNamesW(m, t, CarveNameW, 0);
    return TRUE;
}
static int CarveOne(LPCWSTR in, LPCWSTR out, const WCHAR* keepCsv)
{
    // keep list: comma separated (case-insens, #n for numeric)
    WCHAR buf[512]; lstrcpynW(buf, keepCsv, 512);
    g_nKeep = 0;
    WCHAR* ctx = NULL;
    for (WCHAR* tok = wcstok(buf, L",", &ctx); tok && g_nKeep < 32;
         tok = wcstok(NULL, L",", &ctx)) {
        lstrcpynW(g_keep[g_nKeep++], tok, 64);
    }
    if (!CopyFileW(in, out, FALSE)) { wprintf(L"!! copy err %lu\n",
        GetLastError()); return 1; }
    HMODULE m = LoadLibraryExW(out, NULL, LOAD_LIBRARY_AS_DATAFILE |
                               LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) { wprintf(L"!! carve load err %lu\n", GetLastError()); return 1; }
    g_carveH = BeginUpdateResourceW(out, FALSE);
    if (!g_carveH) { wprintf(L"!! beginupdate err %lu\n", GetLastError());
        FreeLibrary(m); return 1; }
    EnumResourceTypesW(m, CarveTypeW, 0);
    FreeLibrary(m);
    if (!EndUpdateResourceW(g_carveH, FALSE)) {
        wprintf(L"!! endupdate err %lu\n", GetLastError()); return 1; }
    wprintf(L"== carved %s (kept only: %s)\n", out, keepCsv);
    return 0;
}
static void ProbeOne(LPCWSTR path)
{
    wprintf(L"== %s\n", path);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        wprintf(L"   size: %lu bytes\n", fad.nFileSizeLow);
    } else {
        wprintf(L"   !! file not found (err %lu)\n", GetLastError());
        return;
    }

    WCHAR szColor[MAX_PATH] = {0}, szSize[MAX_PATH] = {0};
    SetLastError(0);
    HRESULT hr = S_OK;
    __try {
        hr = pGetThemeDefaults(path, szColor, MAX_PATH,
                               szSize, MAX_PATH);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wprintf(L"   GetThemeDefaults      : AV\n");
        hr = E_FAIL;
    }
    wprintf(L"   GetThemeDefaults      : 0x%08lx color='%s' size='%s'\n",
            (unsigned long)hr, szColor, szSize);
    if (g_forceDefaults) {
        lstrcpyW(szColor, L"NormalColor");
        lstrcpyW(szSize, L"NormalSize");
        wprintf(L"   forced color/size     : NormalColor/NormalSize\n");
    }

    UXTHEMEFILE f;
    ZeroMemory(&f, sizeof(f));
    memcpy(f.header, "thmfile", 7);
    memcpy(f.end, "end", 3);

    HANDLE hSharable = NULL, hNonSharable = NULL, hReuse = NULL;
    // real buffers, like a real theme client: NULL+cch0 may be what makes
    // the loader abort on some paths
    WCHAR ssName[64] = {0}, nsName[64] = {0};
    SetLastError(0);
    DWORD av = 0;
    __try {
        hr = ((LoaderLoadTheme_t)pLoaderLoadTheme)(
            0, 0, path, szColor, szSize,
            &hSharable, ssName, 64, &hNonSharable, nsName, 64,
            NULL, &hReuse, 0, 0, FALSE);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        av = GetExceptionCode();
        hr = E_FAIL;
    }
    wprintf(L"   LoaderLoadTheme(18)   : 0x%08lx  sharable=%p nonsharable=%p%s\n",
            (unsigned long)hr, hSharable, hNonSharable,
            av ? L"  <ACCESS-VIOLATION in loader>" : L"");
    if (FAILED(hr) && !av) {
        __try {
            hr = ((LoaderLoadTheme_t_win11)pLoaderLoadTheme)(
                0, 0, path, szColor, szSize,
                &hSharable, ssName, 64, &hNonSharable, nsName, 64,
                NULL, &hReuse, 0, 0);
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            av = GetExceptionCode();
            hr = E_FAIL;
        }
        wprintf(L"   LoaderLoadTheme(17w11): 0x%08lx  sharable=%p nonsharable=%p%s\n",
                (unsigned long)hr, hSharable, hNonSharable,
                av ? L"  <ACCESS-VIOLATION in loader>" : L"");
    }
    if (FAILED(hr)) {
        wprintf(L"   -> load FAILED, GetLastError=%lu\n", GetLastError());
        return;
    }

    f.hSharableSection = hSharable;
    f.hNsSection = hNonSharable;
    f.sharableSectionView = hSharable
        ? MapViewOfFile(hSharable, FILE_MAP_READ, 0, 0, 0) : NULL;
    f.nsSectionView = hNonSharable
        ? MapViewOfFile(hNonSharable, FILE_MAP_READ, 0, 0, 0) : NULL;
    wprintf(L"   map views             : share=%p noshare=%p (err %lu)\n",
            f.sharableSectionView, f.nsSectionView, GetLastError());

    const WCHAR* classes[] = { L"TASKBAR", L"TASKBAND", L"STARTPANEL",
                               L"TRAYNOTIFY", L"GLOBALS", L"MENU" };
    for (int i = 0; i < _countof(classes); i++) {
        HTHEME ht = pOpenThemeDataFromFile(&f, NULL, classes[i], 0);
        wprintf(L"   OpenThemeDataFromFile(%-11s): %s\n", classes[i],
                ht ? L"OK" : L"NULL");
        if (ht) {
            COLORREF cr = 0;
            HRESULT hrc =
                GetThemeColor(ht, 1 /*TBP_BACKGROUNDBOTTOM*/, 0,
                              3802 /*TMT_FILLCOLOR*/, &cr);
            wprintf(L"      GetThemeColor(1,0,FILLCOLOR): 0x%08lx "
                    L"cr=0x%08lx\n", (unsigned long)hrc,
                    (unsigned long)cr);
        }
    }

    if (f.sharableSectionView) UnmapViewOfFile(f.sharableSectionView);
    if (f.nsSectionView) UnmapViewOfFile(f.nsSectionView);
    if (hSharable) CloseHandle(hSharable);
    if (hNonSharable) CloseHandle(hNonSharable);
}


// ---- structural analysis of a system theme (--analyze) -------------------
// Parses ONLY structural metadata (record headers, ids, sizes, counts,
// hex dumps of structural blobs). Never prints string VALUES from string
// properties: class names are interface identifiers (like API names).
#include <stdint.h>

#pragma pack(push, 1)
struct PropHeader { int32_t nameID, typeID, classID, partID, stateID,
                    shortFlag, reserved, sizeInBytes; };
#pragma pack(pop)

static const uint8_t* ResBytes(HMODULE m, LPCWSTR type, LPCWSTR name,
                               DWORD* size)
{
    HRSRC h = FindResourceW(m, name, type);
    if (!h) return NULL;
    HGLOBAL g = LoadResource(m, h);
    *size = SizeofResource(m, h);
    return g ? (const uint8_t*)LockResource(g) : NULL;
}

static void HexDump(const uint8_t* p, int n)
{
    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) wprintf(L"\n        %04x: ", i);
        wprintf(L"%02x ", p[i]);
    }
    wprintf(L"\n");
}

static void AnalyzeVariant(const uint8_t* v, DWORD size)
{
    wprintf(L"   VARIANT %lu bytes; first 96B:", (unsigned long)size);
    HexDump(v, size < 96 ? (int)size : 96);
    // structural histogram of record headers (port of the public
    // PropertyStream walk: 32B header + data, padded to 8 bytes)
    int nRec = 0, walkErr = 0;
    int typeHist[256] = {0};
    int minClass = 1 << 30, maxClass = -1;
    DWORD off = 0;
    wprintf(L"   first 20 records (no values):");
    while (off + 32 <= size) {
        const PropHeader* h = (const PropHeader*)(v + off);
        if (h->nameID <= 0 || h->nameID > 5000 || h->typeID <= 0 ||
            h->typeID > 400 || h->classID < 0 || h->classID > 2000)
            { off++; continue; }   // leading junk, like PropertyStream does
        DWORD advance = 32;
        DWORD dataLen = 0;
        switch ((int)h->typeID) {
        case 201: dataLen = (DWORD)h->sizeInBytes; break;         // STRING
        case 205: dataLen = 16; break;                            // MARGINS
        case 206: case 210: case 213: dataLen = 0; break;         // in-header
        case 211: dataLen = (DWORD)h->sizeInBytes; break;         // INTLIST
        case 240: dataLen = (DWORD)h->sizeInBytes; break;         // COLORLIST
        default:  dataLen = (DWORD)h->sizeInBytes; break;         // scalar 4/8
        }
        advance = (32 + dataLen + 7) & ~7u;
        if (nRec < 20)
            wprintf(L"\n     rec#%d off=%5lu name=%d type=%d cls=%d part=%d "
                    L"st=%d shrt=%d size=%ld",
                    nRec, (unsigned long)off, h->nameID, h->typeID,
                    h->classID, h->partID, h->stateID, h->shortFlag,
                    (long)h->sizeInBytes);
        if (h->typeID < 256) typeHist[h->typeID]++;
        if (h->classID < minClass) minClass = h->classID;
        if (h->classID > maxClass) maxClass = h->classID;
        nRec++;
        off += advance;
        if (off > size) { walkErr = 1; break; }
    }
    wprintf(L"\n   records seen: %d (resync bytes consumed, walkErr=%d); "
            L"classID range %d..%d\n   typeID histogram:",
            nRec, walkErr, minClass, maxClass);
    for (int i = 0; i < 256; i++)
        if (typeHist[i]) wprintf(L" %d=%d", i, typeHist[i]);
    wprintf(L"\n   walked %lu / %lu bytes\n",
            (unsigned long)off, (unsigned long)size);
}

static void AnalyzeCmap(const uint8_t* c, DWORD size)
{
    // class names are functional identifiers (interface ids); print them
    wprintf(L"   CMAP %lu bytes; first 24 class names:\n", (unsigned long)size);
    int cls = 0; DWORD start = 0;
    for (DWORD i = 0; i + 1 < size; i += 2) {
        if (c[i] == 0 && c[i + 1] == 0) {
            int wlen = (int)((i - start) / 2);
            if (wlen > 0 && cls < 24)
                wprintf(L"     [%d] '%.*ls'\n", cls, wlen,
                        (const WCHAR*)(c + start));
            cls++;
            start = i + 2;
        }
    }
    wprintf(L"     (total classes: %d)\n", cls);
}

static void AnalyzeOne(LPCWSTR path)
{
    wprintf(L"== analyze %s\n", path);
    HMODULE m = LoadLibraryExW(path, NULL,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) { wprintf(L"   LoadLibraryEx err %lu\n", GetLastError()); return; }
    DWORD sz;
    const uint8_t* cmap = ResBytes(m, L"CMAP", L"CMAP", &sz);
    if (cmap) AnalyzeCmap(cmap, sz);
    const uint8_t* var = ResBytes(m, L"VARIANT", L"NORMAL", &sz);
    if (var) AnalyzeVariant(var, sz);
    const uint8_t* rmap = ResBytes(m, L"RMAP", L"RMAP", &sz);
    if (rmap) { wprintf(L"   RMAP %lu bytes, first 96B:", (unsigned long)sz);
                HexDump(rmap, sz < 96 ? (int)sz : 96); }
    const uint8_t* vmap = ResBytes(m, L"VMAP", L"VMAP", &sz);
    if (vmap) { wprintf(L"   VMAP %lu bytes (structural metadata):",
                        (unsigned long)sz);
                HexDump(vmap, (int)sz); }
    const uint8_t* bc = ResBytes(m, L"BCMAP", L"BCMAP", &sz);
    if (bc) { wprintf(L"   BCMAP %lu bytes, first 64B:", (unsigned long)sz);
              HexDump(bc, sz < 64 ? (int)sz : 64); }
    FreeLibrary(m);
}

// --stripsig <in> <out>: truncate the documented signature trailer block
// (magic 0x84692426 footer) so we can load-test a SIGNED system theme as
// if it were unsigned. Works only inside CI on the runner's own copy.
static void StripSig(LPCWSTR in, LPCWSTR out)
{
    HANDLE fi = CreateFileW(in, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (fi == INVALID_HANDLE_VALUE) { wprintf(L"open err %lu\n",
        GetLastError()); return; }
    DWORD sz = GetFileSize(fi, NULL);
    uint8_t* buf = (uint8_t*)malloc(sz);
    DWORD rd = 0; ReadFile(fi, buf, sz, &rd, NULL); CloseHandle(fi);
    uint32_t magic = 0, sigSize = 0, fileSize = 0;
    memcpy(&magic, buf + sz - 16, 4);
    memcpy(&sigSize, buf + sz - 12, 4);
    memcpy(&fileSize, buf + sz - 8, 4);
    wprintf(L"== stripsig: magic=0x%08lx sigSize=%lu fileSize=%lu actual=%lu\n",
            (unsigned long)magic, (unsigned long)sigSize,
            (unsigned long)fileSize, (unsigned long)sz);
    DWORD newLen = (magic == 0x84692426)
        ? sz - 16 - sigSize : sz;
    HANDLE fo = CreateFileW(out, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, 0, NULL);
    DWORD wr = 0; WriteFile(fo, buf, newLen, &wr, NULL); CloseHandle(fo);
    wprintf(L"   wrote %s (%lu bytes, %s)\n", out, (unsigned long)wr,
            magic == 0x84692426 ? L"signature removed" : L"unchanged copy");
    free(buf);
}

int wmain_below() { return 0; }

int wmain(int argc, wchar_t** argv)
{
    // unbuffered: a crash in the loader must not swallow output
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !lstrcmpiW(argv[1], L"--force-defaults")) {
        g_forceDefaults = TRUE;
        argc--; argv++;
    }
    if (argc > 2 && !lstrcmpiW(argv[1], L"--enum")) {
        EnumResources(argv[2]);
        return 0;
    }
    if (argc > 2 && !lstrcmpiW(argv[1], L"--analyze")) {
        AnalyzeOne(argv[2]);
        return 0;
    }
    if (argc > 2 && !lstrcmpiW(argv[1], L"--pehdr")) {
        for (int i = 2; i < argc; i++) PeHeader(argv[i]);
        return 0;
    }
    if (argc > 3 && !lstrcmpiW(argv[1], L"--carve")) {
        // --carve <in> <out> <keepCsv>
        return CarveOne(argv[2], argv[3], argv[4]);
    }
    if (argc > 3 && !lstrcmpiW(argv[1], L"--stripsig")) {
        StripSig(argv[2], argv[3]);
        return 0;
    }
    HMODULE hUx = LoadLibraryW(L"uxtheme.dll");
    if (!hUx) { wprintf(L"no uxtheme (err %lu)\n", GetLastError()); return 2; }
    pGetThemeDefaults = (GetThemeDefaults_t)GetProcAddress(hUx, (LPCSTR)7);
    pLoaderLoadTheme = GetProcAddress(hUx, (LPCSTR)92);
    pOpenThemeDataFromFile =
        (OpenThemeDataFromFile_t)GetProcAddress(hUx, (LPCSTR)16);
    wprintf(L"uxtheme ord7=%p ord92=%p ord16=%p\n",
            pGetThemeDefaults, pLoaderLoadTheme, pOpenThemeDataFromFile);
    if (!pGetThemeDefaults || !pLoaderLoadTheme || !pOpenThemeDataFromFile)
        return 3;

    for (int i = 1; i < argc; i++)
        ProbeOne(argv[i]);
    return 0;
}
