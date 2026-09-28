// ==WindhawkMod==
// @id              ex7-fake-explorer-path
// @name            Explorer7 fake path (%SystemRoot%\explorer.exe)
// @description     Makes the private 7explorer.exe believe it is running as %SystemRoot%\explorer.exe (spoofs GetModuleFileNameW when hModule==NULL). The file on disk is NOT modified. Technique demonstrated by Anixx's "Fake Explorer path" mod; this is a robust rewrite for the 7explorer PoC.
// @version         0.1.0
// @author          7explorer bootstrap
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer7 fake path (PoC companion)

The Windows 7 explorer.exe assumes it lives in the Windows directory in
several places. Run from a private folder (e.g. C:\ex7test) it would
otherwise misbehave. This mod answers every GetModuleFileNameW(NULL, ...)
with "%SystemRoot%\explorer.exe" instead of the real module path.

- only hModule == NULL is spoofed; every other module keeps the original
  answer from the real API;
- the executable on disk is never touched;
- combined with the "Explorer7 shell launcher" mod it lets the private,
  patched explorer.exe act as the logon shell.

Disable it (and the companion mod), sign out/in, and everything is back.
*/
// ==/WindhawkModReadme==

#include <windhawk_api.h>
#include <windows.h>
#include <string.h>

typedef DWORD (WINAPI *GetModuleFileNameW_t)(HMODULE hModule,
                                             LPWSTR lpFilename, DWORD nSize);
static GetModuleFileNameW_t pOriginalGetModuleFileNameW;

static WCHAR g_fakePath[MAX_PATH * 2];  // %SystemRoot%\explorer.exe (expanded)
static DWORD g_fakeLen;                 // length in chars, w/o NUL

static DWORD WINAPI GetModuleFileNameWHook(HMODULE hModule,
                                           LPWSTR lpFilename, DWORD nSize) {
    if (hModule == NULL) {
        if (!lpFilename || nSize == 0) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (nSize > g_fakeLen) {
            memcpy((void*)lpFilename, (const void*)g_fakePath,
                   (g_fakeLen + 1) * sizeof(wchar_t));  // incl. NUL
            return g_fakeLen;
        }
        // Truncation semantics of the original API since Vista: the string
        // is truncated to nSize-1 chars (NUL-terminated) and ERROR_INSUFFICIENT_BUFFER
        // is set; the return value is nSize.
        memcpy((void*)lpFilename, (const void*)g_fakePath,
               (nSize - 1) * sizeof(wchar_t));
        lpFilename[nSize - 1] = L'\0';
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    return pOriginalGetModuleFileNameW(hModule, lpFilename, nSize);
}

BOOL Wh_ModInit(void) {
    WCHAR systemRoot[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"SystemRoot", systemRoot,
                                        (DWORD)_countof(systemRoot));
    if (got == 0 || got >= _countof(systemRoot)) {
        // Defensive fallback; SystemRoot is always present, but never fail.
        GetWindowsDirectoryW(systemRoot, (DWORD)_countof(systemRoot));
    }
    _snwprintf_s(g_fakePath, _countof(g_fakePath), _TRUNCATE,
                 L"%s\\explorer.exe", systemRoot);
    g_fakeLen = (DWORD)wcslen(g_fakePath);

    Wh_Log(L"ex7-fake-explorer-path: path spoof enabled "
           L"(GetModuleFileNameW(NULL) -> %s)", g_fakePath);

    Wh_SetFunctionHook(
        (void*)GetProcAddress(LoadLibraryW(L"kernelbase.dll"),
                              "GetModuleFileNameW"),
        (void*)GetModuleFileNameWHook,
        (void**)&pOriginalGetModuleFileNameW);
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"ex7-fake-explorer-path: uninit, path spoof OFF");
}
