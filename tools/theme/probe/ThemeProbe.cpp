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
                                LPCWSTR lpName, LONG_PTR lParam)
{
    PrintName(L"name", lpName);
    EnumResourceLanguagesW(hModule, lpType, lpName, EnumLangCB, 0);
    return TRUE;
}

static BOOL CALLBACK EnumTypeCB(HMODULE hModule, LPCWSTR lpType,
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
    HRESULT hr = pGetThemeDefaults(path, szColor, MAX_PATH,
                                   szSize, MAX_PATH);
    wprintf(L"   GetThemeDefaults      : 0x%08lx color='%s' size='%s'\n",
            (unsigned long)hr, szColor, szSize);

    UXTHEMEFILE f;
    ZeroMemory(&f, sizeof(f));
    memcpy(f.header, "thmfile", 7);
    memcpy(f.end, "end", 3);

    HANDLE hSharable = NULL, hNonSharable = NULL, hReuse = NULL;
    hr = ((LoaderLoadTheme_t)pLoaderLoadTheme)(
        0, 0, path, szColor, szSize,
        &hSharable, NULL, 0, &hNonSharable, NULL, 0,
        NULL, &hReuse, 0, 0, FALSE);
    wprintf(L"   LoaderLoadTheme(18)   : 0x%08lx  sharable=%p nonsharable=%p\n",
            (unsigned long)hr, hSharable, hNonSharable);
    if (FAILED(hr)) {
        hr = ((LoaderLoadTheme_t_win11)pLoaderLoadTheme)(
            0, 0, path, szColor, szSize,
            &hSharable, NULL, 0, &hNonSharable, NULL, 0,
            NULL, &hReuse, 0, 0);
        wprintf(L"   LoaderLoadTheme(17w11): 0x%08lx  sharable=%p nonsharable=%p\n",
                (unsigned long)hr, hSharable, hNonSharable);
    }
    if (FAILED(hr)) {
        wprintf(L"   -> load FAILED (signature?), GetLastError=%lu\n",
                GetLastError());
        return;
    }

    f.hSharableSection = hSharable;
    f.hNsSection = hNonSharable;
    f.sharableSectionView = MapViewOfFile(hSharable, FILE_MAP_READ, 0, 0, 0);
    f.nsSectionView = MapViewOfFile(hNonSharable, FILE_MAP_READ, 0, 0, 0);
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

int wmain(int argc, wchar_t** argv)
{
    // unbuffered: a crash in the loader must not swallow output
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2 && !lstrcmpiW(argv[1], L"--enum")) {
        EnumResources(argv[2]);
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
