// ==WindhawkMod==
// @id              ex7-userinit-shell
// @name            Explorer7 shell launcher (7explorer private explorer.exe)
// @description     Redirects the Winlogon "Shell" registry query that userinit.exe performs at logon to the private, patched+localized explorer.exe produced by ex7selfcontained (default: C:\ex7test\explorer.exe). Nothing is written to the registry; disabling the mod instantly restores the normal shell at the next logon.
// @version         0.1.0
// @author          7explorer bootstrap
// @include         userinit.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer7 shell launcher (PoC)

Proof-of-concept for 7explorer: makes Windows launch the PRIVATE
explorer.exe (patched + localized, produced by `ex7selfcontained.exe`)
as the interactive shell, WITHOUT replacing C:\Windows\explorer.exe and
WITHOUT writing anything to Winlogon in the registry.

How it works: this mod is injected into userinit.exe only. When userinit
asks the registry for the shell (value name "Shell"), the mod answers
with the configured 7explorer path instead of the real registry value.
If the private executable is missing, the query is passed through to the
original API (fail-safe: the normal Windows shell starts).

Test flow:
  1. run ex7selfcontained.exe                          (creates C:\ex7test\explorer.exe)
  2. enable this mod (check the "ExplorerPath" setting matches your --app-dir)
  3. enable the companion mod "Explorer7 fake path"
  4. sign out, sign back in
Revert: disable this mod, sign out/in — the normal Windows shell returns.
Recovery if the new shell crashes: Ctrl+Shift+Esc -> Task Manager ->
Run new task -> "cmd", then disable the mod from Windhawk; or boot to
Safe Mode (Windhawk does not start there) and disable it.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- ExplorerPath: C:\ex7test\explorer.exe
  $name: Explorer7 executable path
  $description: Path to the private explorer.exe produced by ex7selfcontained.exe (environment variables like %SystemDrive% are expanded)
*/
// ==/WindhawkModSettings==

#include <windhawk_api.h>
#include <windows.h>
#include <string.h>

typedef LONG (WINAPI *RegQueryValueExW_t)(HKEY hKey, LPCWSTR lpValueName,
                                          LPDWORD lpReserved, LPDWORD lpType,
                                          LPBYTE lpData, LPDWORD lpcbData);
static RegQueryValueExW_t pOriginalRegQueryValueExW;

static WCHAR g_settingPath[MAX_PATH * 2];   // raw setting (may contain %VAR%)

static DWORD ExpandTarget(WCHAR* out, DWORD outChars) {
    // ExpandEnvironmentStringsW can be called from userinit at logon; the
    // SystemRoot/SystemDrive variables are system-level and available.
    return ExpandEnvironmentStringsW(g_settingPath, out, outChars);
}

static LONG WINAPI RegQueryValueExWHook(HKEY hKey, LPCWSTR lpValueName,
                                        LPDWORD lpReserved, LPDWORD lpType,
                                        LPBYTE lpData, LPDWORD lpcbData) {
    if (lpValueName && *lpValueName &&
        lstrcmpiW(lpValueName, L"Shell") == 0) {
        Wh_Log(L"ex7-userinit-shell: Shell query intercepted (hKey=%p)", hKey);

        WCHAR target[MAX_PATH * 2];
        DWORD chars = ExpandTarget(target, (DWORD)_countof(target));
        if (chars == 0 || chars >= _countof(target)) {
            Wh_Log(L"ex7-userinit-shell: ERROR expanding '%s' -> "
                   L"original API fallback", g_settingPath);
            return pOriginalRegQueryValueExW(hKey, lpValueName, lpReserved,
                                             lpType, lpData, lpcbData);
        }
        Wh_Log(L"ex7-userinit-shell: target Explorer path = %s", target);

        DWORD attr = GetFileAttributesW(target);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            Wh_Log(L"ex7-userinit-shell: target MISSING (%s) -> "
                   L"original API fallback", target);
            return pOriginalRegQueryValueExW(hKey, lpValueName, lpReserved,
                                             lpType, lpData, lpcbData);
        }
        Wh_Log(L"ex7-userinit-shell: target exists -> returning it as Shell");

        const DWORD need =
            (DWORD)((wcslen(target) + 1) * sizeof(wchar_t));  // incl. NUL

        if (lpType)
            *lpType = REG_SZ;

        if (!lpcbData) {
            // Contractually unusual (no size pointer); only sane when the
            // caller also passed no buffer. Be conservative: fall through to
            // the original API if a buffer was provided without its size.
            if (lpData)
                return pOriginalRegQueryValueExW(hKey, lpValueName, lpReserved,
                                                 lpType, lpData, lpcbData);
            return ERROR_SUCCESS;
        }
        if (!lpData) {
            // Size-only query: report the required byte count, REG_SZ.
            *lpcbData = need;
            return ERROR_SUCCESS;
        }
        if (*lpcbData < need) {
            Wh_Log(L"ex7-userinit-shell: caller buffer too small "
                   L"(%u < %u bytes) -> ERROR_MORE_DATA", *lpcbData, need);
            *lpcbData = need;
            return ERROR_MORE_DATA;
        }
        memcpy((void*)lpData, (const void*)target, need);
        *lpcbData = need;
        return ERROR_SUCCESS;
    }
    // Every unrelated query goes to the original API untouched.
    return pOriginalRegQueryValueExW(hKey, lpValueName, lpReserved, lpType,
                                     lpData, lpcbData);
}

BOOL Wh_ModInit(void) {
    PCWSTR setting = Wh_GetStringSetting(L"ExplorerPath");
    const WCHAR* chosen =
        (setting && *setting) ? setting : L"C:\\ex7test\\explorer.exe";
    wcsncpy_s(g_settingPath, _countof(g_settingPath), chosen, _TRUNCATE);
    if (setting)
        Wh_FreeStringSetting(setting);

    WCHAR expanded[MAX_PATH * 2];
    if (ExpandTarget(expanded, (DWORD)_countof(expanded)))
        Wh_Log(L"ex7-userinit-shell: init, Shell will point to %s", expanded);
    else
        Wh_Log(L"ex7-userinit-shell: init, WARNING: '%s' does not expand",
               g_settingPath);

    Wh_SetFunctionHook(
        (void*)GetProcAddress(LoadLibraryW(L"kernelbase.dll"),
                              "RegQueryValueExW"),
        (void*)RegQueryValueExWHook,
        (void**)&pOriginalRegQueryValueExW);
    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"ex7-userinit-shell: uninit, Shell redirection OFF");
}
