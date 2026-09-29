// shell_switcher.cpp — 7explorer Shell Switcher (runtime, no logout)
//
// Small native Win32 GUI tool that switches the RUNNING shell process
// between the native Windows Explorer (%SystemRoot%\explorer.exe) and the
// private 7explorer Explorer7 executable (default C:\ex7test\explorer.exe,
// overridable via the EX7_EXPLORER_PATH environment variable).
//
// Runtime switching stays registry-free. The OPTIONAL "start at logon"
// feature (the checkbox, test37) instead sets the standard per-user shell:
//
//   - HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell
//     = the private explorer.exe path. This is the documented per-user way
//     to make a program the shell at logon: no elevation, current user
//     only, fully reversible. The previous value is saved and restored
//     byte-per-byte when the feature is turned off.
//   - a per-user Startup-folder link (7explorer-shell.lnk -> --apply-ex7
//     --logon) is kept as a robust fallback: it verifies the switch really
//     happened (retry with backoff, ~60 s) and restarts the --hotkey
//     resident on success.
//   - a per-user scheduled task ("7explorer Shell Recovery", no elevation)
//     runs --recover-login ~30 s after each logon: if the private shell is
//     not alive it restores the previous Shell value, makes sure SOME shell
//     is running, restarts the --hotkey resident and removes itself.
//
// What it never does (hard scope limits):
//   - never modifies C:\Windows\explorer.exe or any system file;
//   - never touches HKLM, userinit.exe, or the machine-wide Winlogon
//     values — only the per-user HKCU Shell value above;
//   - never writes a value pointing to a missing explorer.exe/wrp64.dll:
//     both files are validated before anything is written.
//
// Process identification: the shell process is the owner of the shell
// desktop window (GetShellWindow). Its executable path is read with
// QueryFullProcessImageNameW — this API asks the kernel, so it is immune
// to the Windhawk "fake explorer path" hook (which only affects
// GetModuleFileNameW *inside* the Explorer7 process). No taskkill, no
// "kill every explorer.exe" logic: only the identified shell process is
// stopped.
//
// Build: native x64, static CRT (/MT) — see shell_switcher.vcxproj.
// Log: %TEMP%\7explorer-switcher.log (logon/recovery/switch diagnostics).

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601  // Win7+ (taskschd, QueryFullProcessImageNameW)

// Themed standard controls (comctl32 v6); no external framework.
// (the whole /manifestdependency value must be double-quoted for the linker,
//  otherwise it splits on spaces and errors with LNK1276)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <oaidl.h>
#include <oleauto.h>
#include <taskschd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "taskschd.lib")

// ---------------------------------------------------------------- data ---

enum ShellKind {
    SHELL_NONE = 0,   // no shell window / no process
    SHELL_NATIVE,     // %SystemRoot%\explorer.exe
    SHELL_EX7,        // 7explorer private explorer.exe
    SHELL_OTHER       // some explorer.exe at an unknown path
};

static WCHAR g_nativePath[1024];  // %SystemRoot%\explorer.exe
static WCHAR g_ex7Path[1024];     // private Explorer7 path
static HINSTANCE g_hInst;
static HFONT g_hFont;

// ---- bilingual UI (English default; Italian for Italian systems, or
// EX7_LANG/--lang override). The switcher itself must also work in English.
typedef enum {
    TR_SUBTITLE, TR_GROUPBOX, TR_NATIVE_NAME, TR_EX7_NAME, TR_NONE_NAME,
    TR_UNKNOWN_NAME, TR_BTN_SWITCH, TR_BTN_CANCEL, TR_BTN_BROWSE,
    TR_CHK_LOGIN, TR_STATUS_CURRENT_FMT, TR_STATUS_TARGET_FMT,
    TR_WARN_CONFIRM, TR_ERR_NF_TARGET_FMT, TR_ERR_REFUSED_FMT,
    TR_INFO_ALREADY, TR_ERR_START_EX7_FMT, TR_ERR_CRITICAL_FMT,
    TR_ERR_START_NATIVE_FMT, TR_ERR_STARTUPLINK_FMT, TR_INFO_STARTUP_OK,
    TR_BROWSE_TITLE, TR_BROWSE_FILTER, TR_ERR_STARTUP_OP_FMT,
    TR_CBO_SYS, TR_CBO_EN, TR_CBO_IT,
    TR_BTN_THEME, TR_THEME_TITLE, TR_THEME_FILTER,
    TR_THEME_OK_FMT, TR_THEME_ERR_FMT,
    TR_ERR_LOGON_VALIDATE_FMT, TR_INFO_LOGON_OFF, TR_ERR_LOGON_TASK_FMT,
    TR_COUNT
} TRID;
static const WCHAR* TR_EN[TR_COUNT] = {
    L"Switches the running Explorer shell at runtime. No logout needed.",
    L"Select Explorer shell",
    L"Native Windows Explorer",
    L"Windows 7 Explorer",
    L"(none detected)",
    L"Unknown explorer (see path)",
    L"Switch", L"Cancel", L"Browse\u2026",
    L"Start Windows 7 Explorer automatically at logon\r\n"
    L"(sets the per-user Shell value, reversible \u2014 details: docs/avvio-al-login.md)",
    L"Current shell: %s\r\nPID %lu \u2014 %s",
    L"Target: %s\r\n%s",
    L"Explorer will be restarted.\r\nUnsaved work may be affected.\r\n\r\nContinue?",
    L"Target executable not found:\r\n%s\r\n\r\nNothing was switched.",
    L"Refusing to stop the current shell: its executable\r\n"
    L"is not a recognized explorer:\r\n%s\r\n\r\nNothing was switched.",
    L"The selected shell is already running.",
    L"Failed to start Windows 7 Explorer:\r\n%s\r\n"
    L"CreateProcess error %lu.\r\n\r\nAttempting to restore the native shell\u2026",
    L"CRITICAL: could not start ANY shell.\r\n"
    L"Native restore failed too (error %lu).\r\n\r\n"
    L"Recovery: Ctrl+Alt+Shift+S opens this switcher; or Ctrl+Shift+Esc \u2192 Task Manager \u2192 Run new task \u2192 %s",
    L"Failed to start the native shell:\r\n%s\r\n"
    L"CreateProcess error %lu.\r\n\r\nRetry starting %s?",
    L"Could not %ls the logon auto-start (error %lu).\r\n"
    L"Nothing else was changed.",
    L"Windows 7 Explorer will start at logon. Applied (all reversible):\r\n"
    L"\u2022 per-user Shell value (HKCU ...\\Winlogon\\Shell) \u2014 previous value saved;\r\n"
    L"\u2022 link in your Startup folder (fallback + hotkey restart);\r\n"
    L"\u2022 recovery task \"7explorer Shell Recovery\" (~30 s after logon:\r\n"
    L"   if the private shell is not alive it restores everything).\r\n"
    L"\r\n"
    L"Uncheck the box to undo all three (previous value restored\r\n"
    L"byte-for-byte). Details: docs/avvio-al-login.md",
    L"Select the private Explorer7 (explorer.exe)",
    L"explorer.exe\0explorer.exe\0All files\0*.*\0",
    L"Startup-folder link operation failed (error %lu).",
    L"Explorer7 UI language: System default",
    L"Explorer7 UI language: English",
    L"Explorer7 UI language: Italiano",
    L"Theme\u2026",
    L"Select YOUR Windows 7 theme file (aero.msstyles)",
    L"Theme files\0*.msstyles\0All files\0*.*\0",
    L"Theme installed to:\r\n%s\r\n\r\nSwitch shell (e.g. native \u2192 Explorer7) to apply it.\r\n"
    L"The file is YOURS: it was only copied locally, nothing downloaded or shared.",
    L"Could not copy the theme file (error %lu):\r\n%s",
    L"Cannot enable logon auto-start:\r\n"
    L"%s\r\n\r\n"
    L"Both explorer.exe and wrp64.dll must exist in that folder\r\n"
    L"(the Shell value is never left pointing to missing files).",
    L"Logon auto-start removed.\r\n"
    L"The previous Shell value was restored and the fallback link\r\n"
    L"and recovery task were deleted.",
    L"The per-user Shell value was set, but the recovery task could not\r\n"
    L"be registered (error %lu).\r\n"
    L"Auto-start still works; you just have no automatic safety net\r\n"
    L"at logon (details: docs/avvio-al-login.md).",
};
static const WCHAR* TR_IT[TR_COUNT] = {
    L"Scambia al volo la shell Explorer attiva. Nessun logout richiesto.",
    L"Seleziona la shell Explorer",
    L"Esplora risorse Windows nativo",
    L"Explorer7 (Windows 7)",
    L"(nessuna rilevata)",
    L"Explorer sconosciuto (vedi path)",
    L"Cambia", L"Annulla", L"Sfoglia\u2026",
    L"Avvia Explorer7 automaticamente al logon\r\n"
    L"(imposta il valore Shell dell'utente, reversibile \u2014 dettagli: docs/avvio-al-login.md)",
    L"Shell attuale: %s\r\nPID %lu \u2014 %s",
    L"Destinazione: %s\r\n%s",
    L"Explorer verr\u00e0 riavviato.\r\nIl lavoro non salvato potrebbe essere perso.\r\n\r\nContinuare?",
    L"Eseguibile di destinazione non trovato:\r\n%s\r\n\r\nNessuna modifica applicata.",
    L"Arresto della shell attuale rifiutato: l'eseguibile\r\n"
    L"non \u00e8 un explorer riconosciuto:\r\n%s\r\n\r\nNessuna modifica applicata.",
    L"La shell selezionata \u00e8 gi\u00e0 in esecuzione.",
    L"Impossibile avviare Explorer 7:\r\n%s\r\n"
    L"CreateProcess errore %lu.\r\n\r\nTentativo di ripristino della shell nativa\u2026",
    L"CRITICO: impossibile avviare QUALSIASI shell.\r\n"
    L"Anche il ripristino nativo \u00e8 fallito (errore %lu).\r\n\r\n"
    L"Ripristino: Ctrl+Alt+Maiusc+S apre questo switcher; oppure Ctrl+Shift+Esc \u2192 Gestione attivit\u00e0 \u2192 Esegui nuova attivit\u00e0 \u2192 %s",
    L"Impossibile avviare la shell nativa:\r\n%s\r\n"
    L"CreateProcess errore %lu.\r\n\r\nRipetere l'avvio di %s?",
    L"Impossibile %ls l'avvio automatico al logon (errore %lu).\r\n"
    L"Nient'altro \u00e8 stato modificato.",
    L"Explorer7 verr\u00e0 avviato al logon. Applicato (tutto reversibile):\r\n"
    L"\u2022 valore Shell per-utente (HKCU ...\\Winlogon\\Shell) \u2014 valore\r\n"
    L"   precedente salvato;\r\n"
    L"\u2022 collegamento in Esecuzione automatica (fallback + riavvio hotkey);\r\n"
    L"\u2022 task di recovery \"7explorer Shell Recovery\" (~30 s dopo il logon:\r\n"
    L"   se la shell privata non \u00e8 viva ripristina tutto).\r\n"
    L"\r\n"
    L"Deseleziona la casella per annullare tutte e tre le cose (il valore\r\n"
    L"precedente viene ripristinato byte per byte). Dettagli: docs/avvio-al-login.md",
    L"Seleziona l'Explorer7 privato (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Tutti i file\0*.*\0",
    L"Operazione sul collegamento in Esecuzione automatica non riuscita (errore %lu).",
    L"Lingua UI di Explorer7: di sistema",
    L"Lingua UI di Explorer7: English",
    L"Lingua UI di Explorer7: Italiano",
    L"Tema\u2026",
    L"Seleziona il TUO file tema di Windows 7 (aero.msstyles)",
    L"File tema\0*.msstyles\0Tutti i file\0*.*\0",
    L"Tema installato in:\r\n%s\r\n\r\nCambia shell (es. nativa \u2192 Explorer7) per applicarlo.\r\n"
    L"Il file resta TUO: \u00e8 stato solo copiato in locale, niente \u00e8 stato scaricato o condiviso.",
    L"Impossibile copiare il file tema (errore %lu):\r\n%s",
    L"Impossibile attivare l'avvio automatico al logon:\r\n"
    L"%s\r\n\r\n"
    L"In quella cartella devono esistere sia explorer.exe sia wrp64.dll\r\n"
    L"(il valore Shell non punta mai a file mancanti).",
    L"Avvio automatico al logon rimosso.\r\n"
    L"Il valore Shell precedente \u00e8 stato ripristinato; il collegamento\r\n"
    L"di fallback e il task di recovery sono stati eliminati.",
    L"Il valore Shell per-utente \u00e8 stato impostato, ma il task di recovery\r\n"
    L"non \u00e8 stato registrato (errore %lu).\r\n"
    L"L'avvio automatico funziona comunque; manca solo la rete di\r\n"
    L"sicurezza automatica al logon (dettagli: docs/avvio-al-login.md).",
};
static BOOL g_uiItalian;   // FALSE = English UI (default)
static const WCHAR* TR(TRID id) { return (g_uiItalian ? TR_IT : TR_EN)[id]; }

#define IDC_RADIO_NATIVE  101
#define IDC_RADIO_EX7     102
#define IDC_BTN_SWITCH    201
#define IDC_BTN_CANCEL    202
#define IDC_BTN_BROWSE    203
#define IDC_CHK_LOGIN     204
#define IDC_CBO_SHLANG    205
#define IDC_BTN_THEME     206
#define IDC_ST_NATPATH    301
#define IDC_ST_EX7PATH    302
#define IDC_ST_CURRENT    303
#define IDC_ST_TARGET     304

#define REFRESH_TIMER_MS  1500

#define STARTUP_LINK_NAME L"7explorer-shell.lnk"

// ---- logon auto-start (test37) -------------------------------------------
// Three cooperating pieces, all per-user, all reversible (details in
// docs/avvio-al-login.md):
//   1. HKCU\...\Winlogon\Shell = private explorer.exe  (primary mechanism)
//   2. Startup-folder link -> --apply-ex7 --logon       (fallback, robust)
//   3. scheduled task "7explorer Shell Recovery"        (safety net)
static const WCHAR kWinlogonKey[]  = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
static const WCHAR kShellValue[]   = L"Shell";
static const WCHAR kBackupValue[]  = L"7explorerShellBackup";  // REG_BINARY blob
static const WCHAR kMarkerValue[]  = L"7explorerShellPath";    // REG_SZ we wrote
static const WCHAR kRecoveryTaskName[] = L"7explorer Shell Recovery";
static const WCHAR kApplyRunningMutex[] = L"Local\\7explorer.LogonApplyRunning";
static const DWORD kShellBackupAbsent = 0xFFFFFFFF;  // "value did not exist"

// ------------------------------------------------------------ helpers ---

static BOOL FileExists(LPCWSTR path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ------------------------------------------------------------- logging ---
// Tiny append-only log for the logon/recovery/switch paths (the wrapper
// logs to %TEMP%\7explorer-shellfix.log; this is the switcher's own trail,
// same folder so both are found together). Rotated when it grows too big.
static void SwLogV(const WCHAR* fmt, va_list ap) {
    WCHAR body[900];
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    WCHAR dir[MAX_PATH];
    DWORD n = GetTempPathW((DWORD)_countof(dir), dir);
    if (n == 0 || n >= _countof(dir)) { OutputDebugStringW(body); return; }
    WCHAR path[MAX_PATH + 40];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s7explorer-switcher.log", dir);
    // keep it bounded: start over past ~512 KB
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { OutputDebugStringW(body); return; }
    if (GetFileSize(h, NULL) > 512 * 1024)
        SetFilePointer(h, 0, NULL, FILE_BEGIN), SetEndOfFile(h);
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR line[1024];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"[%02u:%02u:%02u %u] %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), body);
    DWORD cb = (DWORD)(lstrlenW(line) * sizeof(WCHAR));
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(h, line, cb, &written, NULL);
    CloseHandle(h);
    OutputDebugStringW(line);
}
static void SwLog(const WCHAR* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    SwLogV(fmt, ap);
    va_end(ap);
}

// defined in the hotkey section below; used by the logon/recovery paths too
static void EnsureHotkeyResident(void);

// Resolve the two shell paths once. Private path priority (no hardcode):
//   1. EX7_EXPLORER_PATH environment variable (expands %VAR%);
//   2. explorer.exe located NEXT TO this switcher executable (the zip bundle
//      layout: everything extracted into the same folder);
//   3. C:\ex7test\explorer.exe (legacy default/fallback of the bootstrap).
static void ResolveShellPaths(void) {
    WCHAR tmp[1024];

    // Native: %SystemRoot%\explorer.exe
    DWORD got = GetEnvironmentVariableW(L"SystemRoot", tmp,
                                        (DWORD)_countof(tmp));
    if (got == 0 || got >= _countof(tmp))
        GetWindowsDirectoryW(tmp, (DWORD)_countof(tmp));  // fallback
    _snwprintf_s(g_nativePath, _countof(g_nativePath), _TRUNCATE,
                 L"%s\\explorer.exe", tmp);

    // Private, 1) environment override.
    got = GetEnvironmentVariableW(L"EX7_EXPLORER_PATH", tmp,
                                  (DWORD)_countof(tmp));
    if (got > 0 && got < _countof(tmp)) {
        DWORD c = ExpandEnvironmentStringsW(tmp, g_ex7Path,
                                            (DWORD)_countof(g_ex7Path));
        if (c > 0 && c < _countof(g_ex7Path))
            return;
    }

    // Private, 2) side-by-side with this executable.
    WCHAR own[1024];
    DWORD n = GetModuleFileNameW(NULL, own, (DWORD)_countof(own));
    if (n > 0 && n < _countof(own)) {
        WCHAR* bs = wcsrchr(own, L'\\');
        if (bs) {
            *bs = L'\0';
            _snwprintf_s(tmp, _countof(tmp), _TRUNCATE,
                         L"%s\\explorer.exe", own);
            if (FileExists(tmp)) {
                wcsncpy_s(g_ex7Path, _countof(g_ex7Path), tmp, _TRUNCATE);
                return;
            }
        }
    }

    // Private, 3) legacy fallback.
    wcsncpy_s(g_ex7Path, _countof(g_ex7Path), L"C:\\ex7test\\explorer.exe",
              _TRUNCATE);
}

// PID of the process that owns the shell desktop window (the actual shell).
static BOOL GetShellProcessId(DWORD* pPid) {
    HWND hShell = GetShellWindow();
    if (!hShell)
        return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hShell, &pid);
    if (!pid)
        return FALSE;
    *pPid = pid;
    return TRUE;
}

// Executable image path of a process (kernel-provided, hook-immune).
static BOOL QueryProcessExePath(DWORD pid, LPWSTR outPath, DWORD outChars) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return FALSE;
    DWORD size = outChars;
    BOOL ok = QueryFullProcessImageNameW(h, 0, outPath, &size);
    CloseHandle(h);
    return ok;
}

static ShellKind ClassifyPath(LPCWSTR path) {
    if (!path || !*path)
        return SHELL_NONE;
    if (lstrcmpiW(path, g_nativePath) == 0)
        return SHELL_NATIVE;
    if (lstrcmpiW(path, g_ex7Path) == 0)
        return SHELL_EX7;
    // Any other executable ending in explorer.exe (non-shell instance or an
    // unexpected path) — reported, never killed.
    size_t n = wcslen(path);
    const WCHAR suffix[] = L"\\explorer.exe";
    size_t s = _countof(suffix) - 1;
    if (n > s && lstrcmpiW(path + (n - s), suffix) == 0)
        return SHELL_OTHER;
    return SHELL_OTHER;
}

static LPCWSTR KindName(ShellKind k) {
    switch (k) {
    case SHELL_NATIVE: return TR(TR_NATIVE_NAME);
    case SHELL_EX7:    return TR(TR_EX7_NAME);
    case SHELL_NONE:   return TR(TR_NONE_NAME);
    default:           return TR(TR_UNKNOWN_NAME);
    }
}

// Current shell: which executable owns the shell window right now.
static ShellKind DetectCurrentShell(LPWSTR outPath, DWORD outChars,
                                    DWORD* outPid) {
    DWORD pid = 0;
    outPath[0] = L'\0';
    if (outPid) *outPid = 0;
    if (!GetShellProcessId(&pid))
        return SHELL_NONE;
    if (outPid) *outPid = pid;
    if (!QueryProcessExePath(pid, outPath, outChars))
        return SHELL_NONE;
    return ClassifyPath(outPath);
}

// Update the "Current shell" status line.
static void RefreshStatus(HWND hwnd) {
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);

    WCHAR line[1200];
    if (k == SHELL_NONE) {
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     TR(TR_STATUS_CURRENT_FMT), KindName(k),
                     (unsigned long)0, L"-");
    } else {
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     TR(TR_STATUS_CURRENT_FMT),
                     KindName(k), (unsigned long)pid, path);
    }
    SetDlgItemTextW(hwnd, IDC_ST_CURRENT, line);
}

// Which radio (target) the user has selected.
static ShellKind SelectedTarget(HWND hwnd) {
    if (IsDlgButtonChecked(hwnd, IDC_RADIO_EX7) == BST_CHECKED)
        return SHELL_EX7;
    return SHELL_NATIVE;
}

static void UpdateTargetLabel(HWND hwnd) {
    ShellKind t = SelectedTarget(hwnd);
    WCHAR line[1100];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 TR(TR_STATUS_TARGET_FMT), KindName(t),
                 (t == SHELL_EX7) ? g_ex7Path : g_nativePath);
    SetDlgItemTextW(hwnd, IDC_ST_TARGET, line);
}

static void SetRadioForKind(HWND hwnd, ShellKind k) {
    CheckRadioButton(hwnd, IDC_RADIO_NATIVE, IDC_RADIO_EX7,
                     (k == SHELL_EX7) ? IDC_RADIO_EX7 : IDC_RADIO_NATIVE);
}

// ------------------------------------------------------- process ctl ---

// Gracefully stop the shell process: ask it to quit, wait a short timeout,
// then terminate if still alive. Only the identified shell PID is stopped.
static void StopShellProcess(DWORD pid, HWND shellWnd) {
    if (shellWnd && IsWindow(shellWnd))
        PostMessageW(shellWnd, WM_QUIT, 0, 0);   // graceful request

    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!h)
        return;
    if (WaitForSingleObject(h, 2500) != WAIT_OBJECT_0) {
        TerminateProcess(h, 1);                  // timeout: terminate
        WaitForSingleObject(h, 2000);
    }
    CloseHandle(h);
}

// Start an executable as a detached new process. envLang (may be NULL) sets
// EX7_UI_LANG in the child's inherited environment (per-process UI language
// override, honored by wrp64.dll). Returns FALSE+error on immediate failure.
static BOOL LaunchExe(LPCWSTR path, DWORD* pErr, LPCWSTR envLang) {
    WCHAR cmd[1200];
    wcsncpy_s(cmd, _countof(cmd), path, _TRUNCATE);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // Temporarily export EX7_UI_LANG for the child, then restore. The child
    // receives a COPY of the environment at CreateProcess time.
    WCHAR prev[32];
    DWORD had = GetEnvironmentVariableW(L"EX7_UI_LANG", prev, 30);
    if (envLang)
        SetEnvironmentVariableW(L"EX7_UI_LANG", envLang);
    BOOL ok = CreateProcessW(path, cmd, NULL, NULL, FALSE,
                             CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
    if (envLang) {
        if (had > 0 && had < 30)
            SetEnvironmentVariableW(L"EX7_UI_LANG", prev);
        else
            SetEnvironmentVariableW(L"EX7_UI_LANG", NULL);
    }
    if (!ok) {
        if (pErr) *pErr = GetLastError();
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (pErr) *pErr = 0;
    return TRUE;
}

// UI-language selection for the EX7 launch: from the GUI combo (0/1/2) or
// from the caller environment (headless CLI inherits EX7_UI_LANG as-is).
static LPCWSTR SelectedShellUILang(HWND hwnd) {
    if (!hwnd) {
        WCHAR cur[32];
        DWORD n = GetEnvironmentVariableW(L"EX7_UI_LANG", cur, 31);
        return (n > 0 && n < 31) ? L"ENV" : NULL;  // handled inside LaunchExe
    }
    int sel = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_SHLANG),
                                CB_GETCURSEL, 0, 0);
    if (sel == 1) return L"en-US";
    if (sel == 2) return L"it-IT";
    return NULL;  // system default
}

// ---------------------------------------------------------- switching ---

// headless: TRUE when invoked from the command line (--apply-*); suppresses
// informational popups, keeps error popups (message boxes work with hwnd
// NULL). silent: additionally suppresses ALL popups (background/logon use —
// errors are reported via the log and the exit code only).
// Returns 0 on success, 2 on failure (used as process exit code).
static int DoSwitch(HWND hwnd, ShellKind target, BOOL headless, BOOL silent) {
    const WCHAR* targetPath = (target == SHELL_EX7) ? g_ex7Path
                                                    : g_nativePath;

    // 1. The target must exist BEFORE anything is stopped.
    if (!FileExists(targetPath)) {
        WCHAR msg[1300];
        SwLog(L"switch to %s: target not found", targetPath);
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     TR(TR_ERR_NF_TARGET_FMT), targetPath);
        if (!silent)
            MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                        MB_OK | MB_ICONERROR |
                        (headless ? MB_SYSTEMMODAL : 0));
        return 2;
    }

    // 2. Identify the CURRENT shell owner before stopping anything.
    WCHAR curPath[1024];
    DWORD pid = 0;
    ShellKind curKind = DetectCurrentShell(curPath, (DWORD)_countof(curPath),
                                           &pid);

    if (curKind == SHELL_OTHER) {
        // Safety: refuse to stop an unrecognized shell.
        WCHAR msg[1400];
        SwLog(L"refusing to stop unrecognized shell owner %s", curPath);
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     TR(TR_ERR_REFUSED_FMT), curPath);
        if (!silent)
            MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                        MB_OK | MB_ICONERROR |
                        (headless ? MB_SYSTEMMODAL : 0));
        return 2;
    }

    if (curKind == target && pid != 0) {
        if (!headless)
            MessageBoxW(hwnd, TR(TR_INFO_ALREADY),
                        L"7explorer Shell Switcher",
                        MB_OK | MB_ICONINFORMATION);
        if (hwnd) RefreshStatus(hwnd);
        return 0;
    }

    // 3. Stop the current shell (graceful, then terminate after timeout).
    SwLog(L"switch: stopping shell pid %u (%s) -> %s",
          pid, curPath, targetPath);
    HWND shellWnd = GetShellWindow();
    if (pid != 0)
        StopShellProcess(pid, shellWnd);
    Sleep(300);  // let window classes / desks settle

    // 4. Start the target shell. On failure: restore the native shell and
    //    report; never intentionally leave the user without a shell.
    UINT mbExtra = headless ? MB_SYSTEMMODAL : 0;
    DWORD err = 0;
    int failed = 0;
    LPCWSTR envLang = (target == SHELL_EX7) ? SelectedShellUILang(hwnd) : NULL;
    if (envLang && lstrcmpiW(envLang, L"ENV") == 0)
        envLang = NULL;  // headless: keep caller environment as-is
    if (!LaunchExe(targetPath, &err, envLang)) {
        WCHAR msg[1500];
        failed = 1;
        SwLog(L"switch: CreateProcess(%s) failed with error %lu",
              targetPath, (unsigned long)err);
        if (target != SHELL_NATIVE && FileExists(g_nativePath)) {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         TR(TR_ERR_START_EX7_FMT),
                         targetPath, (unsigned long)err);
            if (!silent)
                MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                            MB_OK | MB_ICONERROR | mbExtra);
            DWORD err2 = 0;
            if (!LaunchExe(g_nativePath, &err2, NULL)) {
                SwLog(L"CRITICAL: native restore failed too (error %lu)",
                      (unsigned long)err2);
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             TR(TR_ERR_CRITICAL_FMT),
                             (unsigned long)err2, g_nativePath);
                if (!silent)
                    MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                                MB_OK | MB_ICONSTOP | mbExtra);
            } else {
                failed = 2;  // native restored, but the requested switch failed
            }
        } else {
            int r;
            do {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             TR(TR_ERR_START_NATIVE_FMT),
                             targetPath, (unsigned long)err, targetPath);
                r = silent ? IDOK :
                    MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                                (headless ? MB_OK : MB_RETRYCANCEL) |
                                MB_ICONERROR | mbExtra);
                if (r == IDRETRY && LaunchExe(targetPath, &err, NULL)) {
                    failed = 0;
                    break;
                }
            } while (!headless && !silent && r == IDRETRY);
        }
    }

    // 5. Reflect the new state (GUI only; headless just returns).
    Sleep(800);
    if (hwnd) {
        RefreshStatus(hwnd);
        SetRadioForKind(hwnd, DetectCurrentShell(curPath,
                                                 (DWORD)_countof(curPath),
                                                 &pid));
        UpdateTargetLabel(hwnd);
    }
    return failed ? 2 : 0;
}

// --------------------------------------------------- startup folder ----
// Optional login-time auto-switch FALLBACK (mechanism B in
// docs/avvio-al-login.md): a link in the per-user Startup folder pointing
// at this tool with --apply-ex7 --logon (robust retry + verification +
// hotkey restart). File-based only, no registry, trivially removable.
// Requires CoInitialize by the caller path.

static BOOL StartupLinkPath(LPWSTR outPath, DWORD outChars) {
    WCHAR startup[MAX_PATH];
    if (!SHGetSpecialFolderPathW(NULL, startup, CSIDL_STARTUP, FALSE))
        return FALSE;
    _snwprintf_s(outPath, outChars, _TRUNCATE, L"%s\\%s", startup,
                 STARTUP_LINK_NAME);
    return TRUE;
}

static HRESULT StartupSetPresence(BOOL present, DWORD* pWin32Err) {
    WCHAR linkPath[MAX_PATH];
    if (!StartupLinkPath(linkPath, (DWORD)_countof(linkPath))) {
        if (pWin32Err) *pWin32Err = GetLastError();
        return E_FAIL;
    }
    if (!present) {
        if (FileExists(linkPath) && !DeleteFileW(linkPath)) {
            if (pWin32Err) *pWin32Err = GetLastError();
            return E_FAIL;
        }
        if (pWin32Err) *pWin32Err = ERROR_SUCCESS;
        return S_OK;
    }
    // present: create <Startup>\7explorer-shell.lnk ->
    //          "<own exe>" --apply-ex7
    WCHAR own[1024];
    if (!GetModuleFileNameW(NULL, own, (DWORD)_countof(own))) {
        if (pWin32Err) *pWin32Err = GetLastError();
        return E_FAIL;
    }
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL uninit = SUCCEEDED(hr);
    if (uninit || hr == RPC_E_CHANGED_MODE) {
        IShellLinkW* sl = NULL;
        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW, (void**)&sl);
        if (SUCCEEDED(hr)) {
            sl->SetPath(own);
            sl->SetArguments(L"--apply-ex7 --logon");
            sl->SetDescription(L"7explorer shell at logon (fallback)");
            IPersistFile* pf = NULL;
            hr = sl->QueryInterface(IID_IPersistFile, (void**)&pf);
            if (SUCCEEDED(hr)) {
                hr = pf->Save(linkPath, TRUE);
                pf->Release();
            }
            sl->Release();
        }
        if (uninit)
            CoUninitialize();
    }
    if (pWin32Err) *pWin32Err = (DWORD)hr;
    return hr;
}

static BOOL StartupIsPresent(void) {
    WCHAR linkPath[MAX_PATH];
    return StartupLinkPath(linkPath, (DWORD)_countof(linkPath)) &&
           FileExists(linkPath);
}

// ------------------------------------------- per-user Winlogon Shell -----
// Mechanism (A) of the logon auto-start: the standard per-user shell value
// HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell. The
// previous value is saved (type + raw bytes, i.e. byte-for-byte) and
// restored on removal. A REG_SZ marker records the exact string we wrote so
// "ours" detection never guesses. Nothing is ever written if the private
// explorer.exe + wrp64.dll do not exist.

// Read the raw Shell value. Returns TRUE when the value exists.
static BOOL ReadShellRaw(BYTE* buf, DWORD bufCb, DWORD* pDataCb,
                         DWORD* pType) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, KEY_READ, &k)
        != ERROR_SUCCESS)
        return FALSE;
    DWORD cb = bufCb, type = 0;
    LONG r = RegQueryValueExW(k, kShellValue, NULL, &type, buf, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA)
        return FALSE;
    if (pDataCb) *pDataCb = cb;
    if (pType) *pType = type;
    return TRUE;
}

// Does the Shell value still contain exactly what we last wrote? The
// marker value records our string verbatim, so detection never guesses.
static BOOL MarkerMatchesShellValue(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, KEY_READ, &k)
        != ERROR_SUCCESS)
        return FALSE;
    WCHAR marker[1100];
    DWORD cb = sizeof(marker);
    BOOL ok = FALSE;
    if (RegQueryValueExW(k, kMarkerValue, NULL, NULL, (LPBYTE)marker, &cb)
        == ERROR_SUCCESS && cb >= 2) {
        WCHAR shell[1100];
        cb = sizeof(shell);
        if (RegQueryValueExW(k, kShellValue, NULL, NULL, (LPBYTE)shell, &cb)
            == ERROR_SUCCESS && cb >= 2)
            ok = (lstrcmpiW(marker, shell) == 0);
    }
    RegCloseKey(k);
    return ok;
}

// Save the current Shell value (type + bytes) into kBackupValue.
// kShellBackupAbsent encodes "the value did not exist".
static BOOL SaveShellBackup(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, NULL, 0,
                        KEY_READ | KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return FALSE;
    BYTE buf[2048], blob[2048 + 16];
    DWORD cb = 0, type = 0;
    BOOL exists = ReadShellRaw(buf, sizeof(buf), &cb, &type);
    if (exists && (cb > sizeof(buf) || cb < 2)) {
        // oversized/garbage value: refuse to trade it away unsaved
        SwLog(L"logon: previous Shell value too large (%lu bytes) - "
              L"refusing to enable", (unsigned long)cb);
        RegCloseKey(k);
        return FALSE;
    }
    DWORD hdr[3];
    hdr[0] = 1;  // format version
    hdr[1] = exists ? cb : kShellBackupAbsent;
    hdr[2] = type;
    memcpy(blob, hdr, sizeof(hdr));
    if (exists)
        memcpy(blob + sizeof(hdr), buf, cb);
    LONG r = RegSetValueExW(k, kBackupValue, 0, REG_BINARY, blob,
                            sizeof(hdr) + (exists ? cb : 0));
    RegCloseKey(k);
    SwLog(L"logon: saved previous Shell value (%s, %lu bytes)",
          exists ? L"present" : L"absent", exists ? (unsigned long)cb : 0UL);
    return r == ERROR_SUCCESS;
}

// Write the per-user Shell value = private explorer.exe (quoted when the
// path contains spaces, as Winlogon-style values are parsed with quotes).
static BOOL WriteShellValue(LPCWSTR path, LPWSTR outWritten, DWORD outChars) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, NULL, 0,
                        KEY_READ | KEY_SET_VALUE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return FALSE;
    WCHAR v[1100];
    if (wcschr(path, L' '))
        _snwprintf_s(v, _countof(v), _TRUNCATE, L"\"%s\"", path);
    else
        wcsncpy_s(v, _countof(v), path, _TRUNCATE);
    LONG r = RegSetValueExW(k, kShellValue, 0, REG_SZ, (const BYTE*)v,
                            (DWORD)((lstrlenW(v) + 1) * sizeof(WCHAR)));
    BOOL ok = r == ERROR_SUCCESS;
    if (ok) {
        r = RegSetValueExW(k, kMarkerValue, 0, REG_SZ, (const BYTE*)v,
                           (DWORD)((lstrlenW(v) + 1) * sizeof(WCHAR)));
        ok = r == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    if (ok && outWritten)
        wcsncpy_s(outWritten, outChars, v, _TRUNCATE);
    SwLog(L"logon: HKCU Winlogon Shell = %s (%s)", v,
          ok ? L"ok" : L"WRITE FAILED");
    return ok;
}

// Restore the backed-up Shell value byte-for-byte (deleting it when it did
// not exist before us). Returns TRUE when the value is ours no longer.
static BOOL RestoreShellValue(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0,
                      KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    BYTE blob[2048 + 16];
    DWORD cb = sizeof(blob);
    LONG r = RegQueryValueExW(k, kBackupValue, NULL, NULL, blob, &cb);
    BOOL restored = FALSE;
    if (r == ERROR_SUCCESS && cb >= sizeof(DWORD) * 3) {
        DWORD* hdr = (DWORD*)blob;
        if (hdr[0] == 1) {
            if (hdr[1] == kShellBackupAbsent) {
                restored = RegDeleteValueW(k, kShellValue) == ERROR_SUCCESS;
                SwLog(L"logon: Shell value removed (it did not exist before)");
            } else if (hdr[1] <= cb - sizeof(DWORD) * 3 &&
                       hdr[1] <= 2048) {
                restored = RegSetValueExW(k, kShellValue, 0, hdr[2],
                                          blob + sizeof(DWORD) * 3,
                                          hdr[1]) == ERROR_SUCCESS;
                SwLog(L"logon: Shell value restored byte-for-byte "
                      L"(%lu bytes, type %lu)", (unsigned long)hdr[1],
                      (unsigned long)hdr[2]);
            }
        }
    }
    RegDeleteValueW(k, kMarkerValue);
    RegDeleteValueW(k, kBackupValue);
    RegCloseKey(k);
    return restored;
}

// Enable mechanism (A). Files are validated FIRST: the value must never
// point at a missing explorer.exe/wrp64.dll.
static BOOL LogonRegistryEnable(LPWSTR pErr, DWORD errChars) {
    pErr[0] = L'\0';
    if (!FileExists(g_ex7Path)) {
        _snwprintf_s(pErr, errChars, _TRUNCATE, L"%s", g_ex7Path);
        return FALSE;
    }
    WCHAR wrp[1100];
    wcsncpy_s(wrp, _countof(wrp), g_ex7Path, _TRUNCATE);
    WCHAR* bs = wcsrchr(wrp, L'\\');
    if (bs)
        *bs = L'\0';
    else
        wrp[0] = L'\0';
    lstrcatW(wrp, L"\\wrp64.dll");
    if (!FileExists(wrp)) {
        _snwprintf_s(pErr, errChars, _TRUNCATE, L"%s", wrp);
        return FALSE;
    }
    if (!MarkerMatchesShellValue() && !SaveShellBackup())
        return FALSE;  // never trade the old value away without a backup
    WCHAR written[1100];
    return WriteShellValue(g_ex7Path, written, (DWORD)_countof(written));
}

// Remove mechanism (A): restore the previous value. When the current value
// is no longer the one we wrote (another tool owns it now), it is left
// untouched and only our bookkeeping values are deleted.
static BOOL LogonRegistryDisable(void) {
    if (MarkerMatchesShellValue())
        return RestoreShellValue();
    SwLog(L"logon: Shell value changed by someone else - left untouched");
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0,
                      KEY_READ | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kMarkerValue);
        RegDeleteValueW(k, kBackupValue);
        RegCloseKey(k);
    }
    return TRUE;
}

static BOOL LogonRegistryActive(void) {
    if (MarkerMatchesShellValue())
        return TRUE;
    // marker present but Shell changed externally: still "we are installed"
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, KEY_READ, &k)
        != ERROR_SUCCESS)
        return FALSE;
    DWORD cb = 0;
    LONG r = RegQueryValueExW(k, kMarkerValue, NULL, NULL, NULL, &cb);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

// ------------------------------------- scheduled recovery task (COM) ----
// Per-user, no elevation: LogonTrigger (30 s delay) + ExecAction running
// this executable with --recover-login. Created while the logon auto-start
// is enabled; removed when it is disabled (and it removes itself if it ever
// has to restore the previous shell). See docs/avvio-al-login.md.

// COM apartment helper: safe on any thread (balanced CoUninitialize).
static BOOL ComApartmentInit(BOOL* pUninit) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    *pUninit = SUCCEEDED(hr);  // S_FALSE still needs the matching uninit
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

static BOOL TaskComInit(ITaskService** pSvc) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, NULL,
                                  CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  (void**)pSvc);
    if (FAILED(hr)) { SwLog(L"recovery task: CoCreateInstance hr=0x%08X", (DWORD)hr); return FALSE; }
    VARIANT v1, v2, v3, v4;
    VariantInit(&v1); VariantInit(&v2); VariantInit(&v3); VariantInit(&v4);
    hr = (*pSvc)->Connect(v1, v2, v3, v4);
    if (FAILED(hr)) {
        SwLog(L"recovery task: Connect hr=0x%08X", (DWORD)hr);
        (*pSvc)->Release();
        *pSvc = NULL;
        return FALSE;
    }
    return TRUE;
}

static BOOL RecoveryTaskExistsInner(void) {
    ITaskService* svc = NULL;
    if (!TaskComInit(&svc)) return FALSE;
    ITaskFolder* root = NULL;
    BOOL found = FALSE;
    BSTR broot = SysAllocString(L"\\");
    BSTR bname = SysAllocString(kRecoveryTaskName);
    if (broot && bname && SUCCEEDED(svc->GetFolder(broot, &root)) && root) {
        IRegisteredTask* t = NULL;
        if (SUCCEEDED(root->GetTask(bname, &t)) && t) {
            found = TRUE;
            t->Release();
        }
        root->Release();
    }
    SysFreeString(broot);
    SysFreeString(bname);
    svc->Release();
    return found;
}

static BOOL RecoveryTaskExists(void) {
    BOOL uninit = FALSE, ok = FALSE;
    if (ComApartmentInit(&uninit))
        ok = RecoveryTaskExistsInner();
    if (uninit) CoUninitialize();
    return ok;
}

static BOOL RecoveryTaskInstallInner(LPWSTR pErr, DWORD errChars) {
    HRESULT hr = E_FAIL;
    ITaskService* svc = NULL;
    ITaskFolder* root = NULL;
    ITaskDefinition* def = NULL;
    IRegisteredTask* regd = NULL;
    BOOL ok = FALSE;
    pErr[0] = L'\0';
    if (!TaskComInit(&svc)) { _snwprintf_s(pErr, errChars, _TRUNCATE, L"COM"); return FALSE; }
    BSTR broot = SysAllocString(L"\\");
    BSTR bname = SysAllocString(kRecoveryTaskName);
    BSTR bpath = NULL, bargs = NULL, bdelay = NULL, blimit = NULL;
    WCHAR own[MAX_PATH];
    GetModuleFileNameW(NULL, own, MAX_PATH);
    do {
        if (!broot || !bname) break;
        if (FAILED(hr = svc->GetFolder(broot, &root))) break;
        if (FAILED(hr = root->NewTask(0, &def))) break;
        ITriggerCollection* trigs = NULL;
        if (FAILED(hr = def->get_Triggers(&trigs))) break;
        ITrigger* trig = NULL;
        hr = trigs->Create(TASK_TRIGGER_LOGON, &trig);
        trigs->Release();
        if (FAILED(hr)) break;
        ILogonTrigger* lt = NULL;
        hr = trig->QueryInterface(IID_ILogonTrigger, (void**)&lt);
        trig->Release();
        if (FAILED(hr)) break;
        bdelay = SysAllocString(L"PT30S");
        if (bdelay) lt->put_Delay(bdelay);
        lt->Release();
        IActionCollection* acts = NULL;
        if (FAILED(hr = def->get_Actions(&acts))) break;
        IAction* act = NULL;
        hr = acts->Create(TASK_ACTION_EXEC, &act);
        acts->Release();
        if (FAILED(hr)) break;
        IExecAction* ea = NULL;
        hr = act->QueryInterface(IID_IExecAction, (void**)&ea);
        act->Release();
        if (FAILED(hr)) break;
        bpath = SysAllocString(own);
        bargs = SysAllocString(L"--recover-login");
        hr = ea->put_Path(bpath);
        if (SUCCEEDED(hr)) hr = ea->put_Arguments(bargs);
        ea->Release();
        if (FAILED(hr)) break;
        IPrincipal* prin = NULL;
        if (FAILED(hr = def->get_Principal(&prin))) break;
        prin->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        prin->put_RunLevel(TASK_RUNLEVEL_LUA);
        prin->Release();
        ITaskSettings* set = NULL;
        if (FAILED(hr = def->get_Settings(&set))) break;
        set->put_StartWhenAvailable(VARIANT_TRUE);
        blimit = SysAllocString(L"PT10M");
        if (blimit) set->put_ExecutionTimeLimit(blimit);
        set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        set->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        set->Release();
        VARIANT v1, v2;
        VariantInit(&v1); VariantInit(&v2);
        hr = root->RegisterTaskDefinition(bname, def, TASK_CREATE_OR_UPDATE,
                                          v1, v2, TASK_LOGON_INTERACTIVE_TOKEN,
                                          v1, &regd);
        if (SUCCEEDED(hr)) ok = TRUE;
    } while (0);
    if (regd) regd->Release();
    if (def) def->Release();
    if (root) root->Release();
    svc->Release();
    SysFreeString(broot);
    SysFreeString(bname);
    SysFreeString(bpath);
    SysFreeString(bargs);
    SysFreeString(bdelay);
    SysFreeString(blimit);
    if (!ok)
        _snwprintf_s(pErr, errChars, _TRUNCATE, L"hr=0x%08X", (DWORD)hr);
    SwLog(L"recovery task: install %s (%s)", ok ? L"ok" : L"FAILED", pErr);
    return ok;
}

static BOOL RecoveryTaskInstall(LPWSTR pErr, DWORD errChars) {
    BOOL uninit = FALSE, ok = FALSE;
    if (ComApartmentInit(&uninit))
        ok = RecoveryTaskInstallInner(pErr, errChars);
    if (uninit) CoUninitialize();
    return ok;
}

static BOOL RecoveryTaskRemoveInner(void) {
    ITaskService* svc = NULL;
    if (!TaskComInit(&svc)) return FALSE;
    ITaskFolder* root = NULL;
    BOOL ok = FALSE;
    BSTR broot = SysAllocString(L"\\");
    BSTR bname = SysAllocString(kRecoveryTaskName);
    if (broot && bname && SUCCEEDED(svc->GetFolder(broot, &root)) && root) {
        HRESULT hr = root->DeleteTask(bname, 0);
        ok = SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        SwLog(L"recovery task: remove %s", ok ? L"ok" : L"FAILED");
        root->Release();
    }
    SysFreeString(broot);
    SysFreeString(bname);
    svc->Release();
    return ok;
}

static BOOL RecoveryTaskRemove(void) {
    BOOL uninit = FALSE, ok = FALSE;
    if (ComApartmentInit(&uninit))
        ok = RecoveryTaskRemoveInner();
    if (uninit) CoUninitialize();
    return ok;
}

// ------------------------------------------------- logon auto-start -----
// The checkbox = all three mechanisms together (registry + fallback link +
// recovery task). Each step is undone on disable; nothing is left behind.

static BOOL LogonAutoStartPresent(void) {
    return LogonRegistryActive() || StartupIsPresent() ||
           RecoveryTaskExists();
}

// Enable. Returns 0 = ok, 2 = refused (missing files, nothing changed),
// 3 = registry set but the recovery task could not be registered.
static int LogonAutoStartEnable(LPWSTR pDetail, DWORD detailChars,
                                DWORD* pTaskErr) {
    pDetail[0] = L'\0';
    if (pTaskErr) *pTaskErr = 0;
    // 1. validate: explorer.exe + wrp64.dll must exist
    if (!LogonRegistryEnable(pDetail, detailChars)) {
        SwLog(L"logon enable: validation failed (%s)", pDetail);
        return 2;
    }
    // 2. recovery task (safety net first: never have (A) without it)
    WCHAR taskErr[64];
    BOOL taskOk = RecoveryTaskInstall(taskErr, (DWORD)_countof(taskErr));
    if (!taskOk && pTaskErr) {
        // taskErr looks like "hr=0x80070005" (or "COM"): extract the hex
        const WCHAR* hx = wcsrchr(taskErr, L'x');
        *pTaskErr = hx ? (DWORD)wcstoul(hx + 1, NULL, 16) : 0;
    }
    // 3. fallback link
    DWORD le = 0;
    BOOL linkOk = SUCCEEDED(StartupSetPresence(TRUE, &le));
    if (!linkOk)
        SwLog(L"logon enable: startup link failed (error %lu)",
              (unsigned long)le);
    SwLog(L"logon enable: registry ok, task %s, link %s",
          taskOk ? L"ok" : L"FAILED", linkOk ? L"ok" : L"FAILED");
    return taskOk ? 0 : 3;
}

static void LogonAutoStartDisable(void) {
    LogonRegistryDisable();
    RecoveryTaskRemove();
    DWORD e = 0;
    StartupSetPresence(FALSE, &e);
    SwLog(L"logon disable: complete (registry restored, task and link removed)");
}

// --------------------------------------- robust logon --apply-ex7 -------
// Used by the Startup-folder link (--apply-ex7 --logon) and by plain
// --apply-ex7. The switch is attempted and then VERIFIED (the shell window
// owner must really be the private explorer); on failure it retries with
// backoff for about a minute. On success the --hotkey resident is (re)
// started. kApplyRunningMutex tells --recover-login that a switch is still
// being attempted, so the recovery task does not fight it.

static BOOL ApplyMutexHold(HANDLE* pMutex) {
    HANDLE m = CreateMutexW(NULL, TRUE, kApplyRunningMutex);
    *pMutex = m;
    return m != NULL && GetLastError() != ERROR_ALREADY_EXISTS;
}

static int ApplyEx7Robust(BOOL fromLogonLink) {
    static const DWORD waitMs[] = { 5000, 10000, 15000, 20000, 25000 };
    const DWORD totalMs = 60000;
    DWORD spent = 0;
    int rc = 2;
    SwLog(L"apply-ex7 (%s): target %s",
          fromLogonLink ? L"logon link" : L"manual",
          g_ex7Path);
    HANDLE mutex = NULL;
    BOOL ownMutex = fromLogonLink && ApplyMutexHold(&mutex);
    for (int attempt = 1; ; attempt++) {
        WCHAR path[1024];
        DWORD pid = 0;
        ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
        if (k == SHELL_EX7 && pid != 0) {
            SwLog(L"apply-ex7: verified, shell pid %u (%s)", pid, path);
            rc = 0;
            break;
        }
        if (!FileExists(g_ex7Path)) {
            SwLog(L"apply-ex7: target missing, not retrying");
            rc = 2;
            break;
        }
        if (attempt > 1)
            SwLog(L"apply-ex7: attempt %d (shell is %s), retrying",
                  attempt, KindName(k));
        rc = DoSwitch(NULL, SHELL_EX7, TRUE, fromLogonLink /*silent*/);
        // verification happens at the top of the next iteration
        if (spent >= totalMs)
            break;
        int wi = attempt - 1;
        if (wi > (int)_countof(waitMs) - 1)
            wi = (int)_countof(waitMs) - 1;
        DWORD w = waitMs[wi];
        if (spent + w > totalMs)
            w = totalMs - spent;
        Sleep(w);
        spent += w;
    }
    if (rc == 0)
        EnsureHotkeyResident();  // Ctrl+Alt+Shift+S alive after every switch
    else
        SwLog(L"apply-ex7: giving up after ~%lu ms (rc=%d)",
              (unsigned long)spent, rc);
    if (ownMutex && mutex)
        ReleaseMutex(mutex);
    if (mutex)
        CloseHandle(mutex);
    return rc;
}

// ---------------------------------------------- --recover-login ---------
// Body of the scheduled recovery task (runs ~30 s after logon, per-user,
// no UI). If the private shell is alive: nothing to do (the task stays for
// the next logon; wrp64.dll has already restarted the hotkey resident).
// Otherwise: make sure SOME shell is running, restore the previous Shell
// value, remove this task, restart the hotkey resident.
static int RecoverLogin(void) {
    SwLog(L"recover-login: start (checking the private shell)");
    for (int i = 0; i < 5; i++) {  // ~10 s of grace past the 30 s delay
        WCHAR path[1024];
        DWORD pid = 0;
        ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
        if (k == SHELL_EX7 && pid != 0) {
            SwLog(L"recover-login: private shell alive (pid %u) - ok", pid);
            return 0;  // keep the task armed for the next logon
        }
        // a Startup-link switch may still be retrying: do not fight it
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kApplyRunningMutex);
        if (m) {
            SwLog(L"recover-login: logon switch still in progress - ok");
            CloseHandle(m);
            return 0;
        }
        Sleep(2000);
    }
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
    if (k == SHELL_NONE) {
        // nothing owns the desktop: start the native shell right now
        DWORD err = 0;
        if (LaunchExe(g_nativePath, &err, NULL))
            SwLog(L"recover-login: no shell at all - native started");
        else
            SwLog(L"recover-login: no shell at all, native start FAILED"
                  L" (error %lu) - manual recovery: Task Manager -> Run"
                  L" new task -> explorer.exe", (unsigned long)err);
    } else {
        SwLog(L"recover-login: shell is not the private one (%s) - "
              L"restoring the previous configuration", path);
    }
    LogonRegistryDisable();   // byte-for-byte restore of the Shell value
    RecoveryTaskRemove();     // this task self-eliminates
    EnsureHotkeyResident();   // Ctrl+Alt+Shift+S works from now on
    SwLog(L"recover-login: done (auto-start disabled; the Startup link, if "
          L"present, keeps trying as plain fallback)");
    return 0;
}


// ------------------------------------------------------------- theme ----
// Lets the USER pick their own Windows 7 .msstyles (e.g. extracted from a
// copy of their own Windows 7) and copies it next to the private Explorer7
// as <ex7dir>\theme\aero.msstyles, the exact layout the wrapper's
// ThemeManager (and upstream explorer7) expects. If a matching
// <lang>\aero.msstyles.mui sits next to the picked file (en-US and/or
// it-IT), it is copied too. 100% local file copy of a user-owned file:
// nothing is downloaded, embedded or redistributed by this project.

static BOOL CopyThemeLangMui(LPCWSTR srcDir, LPCWSTR dstThemeDir,
                             LPCWSTR langDir) {
    WCHAR src[1024], dstDir[1024], dst[1024];
    _snwprintf_s(src, _countof(src), _TRUNCATE, L"%s\\%s\\aero.msstyles.mui",
                 srcDir, langDir);
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES)
        return TRUE;  // optional: absent is fine, not an error
    _snwprintf_s(dstDir, _countof(dstDir), _TRUNCATE, L"%s\\%s",
                 dstThemeDir, langDir);
    CreateDirectoryW(dstDir, NULL);
    _snwprintf_s(dst, _countof(dst), _TRUNCATE, L"%s\\aero.msstyles.mui",
                 dstDir);
    return CopyFileW(src, dst, FALSE);
}

static void InstallTheme(HWND hwnd) {
    WCHAR src[1024];
    src[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = src;
    ofn.nMaxFile = (DWORD)_countof(src);
    ofn.lpstrFilter = TR(TR_THEME_FILTER);
    ofn.lpstrTitle = TR(TR_THEME_TITLE);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    // destination: <dir of the private explorer7>\theme\aero.msstyles
    WCHAR dst[1024];
    wcsncpy_s(dst, _countof(dst), g_ex7Path, _TRUNCATE);
    WCHAR* bs = wcsrchr(dst, L'\\');
    if (bs) *bs = L'\0';
    lstrcatW(dst, L"\\theme");
    CreateDirectoryW(dst, NULL);
    WCHAR dstThemeDir[1024];
    wcsncpy_s(dstThemeDir, _countof(dstThemeDir), dst, _TRUNCATE);
    lstrcatW(dst, L"\\aero.msstyles");

    WCHAR msg[1400];
    BOOL ok = CopyFileW(src, dst, FALSE);
    DWORD err = ok ? 0 : GetLastError();
    if (ok) {
        // optional per-language .mui sitting next to the picked file
        WCHAR srcDir[1024];
        wcsncpy_s(srcDir, _countof(srcDir), src, _TRUNCATE);
        bs = wcsrchr(srcDir, L'\\');
        if (bs) *bs = L'\0';
        if (!CopyThemeLangMui(srcDir, dstThemeDir, L"en-US") ||
            !CopyThemeLangMui(srcDir, dstThemeDir, L"it-IT")) {
            ok = FALSE;
            err = GetLastError();
        }
    }
    if (ok)
        _snwprintf_s(msg, _countof(msg), _TRUNCATE, TR(TR_THEME_OK_FMT), dst);
    else
        _snwprintf_s(msg, _countof(msg), _TRUNCATE, TR(TR_THEME_ERR_FMT),
                     (unsigned long)err, dst);
    MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

// ------------------------------------------------------------- browse ---

static void BrowseForExplorer7(HWND hwnd) {
    WCHAR file[1024];
    wcsncpy_s(file, _countof(file), g_ex7Path, _TRUNCATE);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)_countof(file);
    ofn.lpstrFilter = TR(TR_BROWSE_FILTER);
    ofn.lpstrTitle = TR(TR_BROWSE_TITLE);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        wcsncpy_s(g_ex7Path, _countof(g_ex7Path), file, _TRUNCATE);
        SetDlgItemTextW(hwnd, IDC_ST_EX7PATH, g_ex7Path);
        UpdateTargetLabel(hwnd);
    }
}

// ------------------------------------------------------------ window ----

static HWND MakeChild(HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style,
                      int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text,
                             style | WS_CHILD | WS_VISIBLE,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (c && g_hFont)
        SendMessageW(c, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return c;
}

static void OnCreate(HWND hwnd) {
    g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    MakeChild(hwnd, WC_STATICW, L"7explorer Shell Switcher",
              SS_CENTER, 10, 10, 540, 22, 0);
    MakeChild(hwnd, WC_STATICW, TR(TR_SUBTITLE),
              SS_CENTER, 10, 32, 540, 16, 0);

    MakeChild(hwnd, WC_BUTTONW, TR(TR_GROUPBOX),
              BS_GROUPBOX, 10, 54, 540, 132, 0);

    MakeChild(hwnd, WC_BUTTONW, TR(TR_NATIVE_NAME),
              BS_AUTORADIOBUTTON | WS_TABSTOP,
              26, 76, 240, 20, IDC_RADIO_NATIVE);
    MakeChild(hwnd, WC_STATICW, g_nativePath,
              SS_LEFT, 44, 97, 490, 16, IDC_ST_NATPATH);

    MakeChild(hwnd, WC_BUTTONW, TR(TR_EX7_NAME),
              BS_AUTORADIOBUTTON | WS_TABSTOP,
              26, 122, 240, 20, IDC_RADIO_EX7);
    MakeChild(hwnd, WC_STATICW, g_ex7Path,
              SS_LEFT, 44, 143, 420, 16, IDC_ST_EX7PATH);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_BROWSE),
              BS_PUSHBUTTON | WS_TABSTOP,
              470, 140, 82, 22, IDC_BTN_BROWSE);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_THEME),
              BS_PUSHBUTTON | WS_TABSTOP,
              330, 290, 96, 22, IDC_BTN_THEME);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_CHK_LOGIN),
              BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP,
              26, 172, 524, 30, IDC_CHK_LOGIN);

    MakeChild(hwnd, WC_STATICW, L"\u2026",
              SS_LEFT, 26, 208, 524, 34, IDC_ST_CURRENT);
    MakeChild(hwnd, WC_STATICW, L"\u2026",
              SS_LEFT, 26, 248, 524, 34, IDC_ST_TARGET);

    HWND cbo = MakeChild(hwnd, WC_COMBOBOXW, NULL,
                         CBS_DROPDOWNLIST | WS_VSCROLL,
                         26, 290, 280, 140, IDC_CBO_SHLANG);
    SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_SYS));
    SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_EN));
    SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_IT));
    SendMessageW(cbo, CB_SETCURSEL, 0, 0);

    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_SWITCH),
              BS_DEFPUSHBUTTON | WS_TABSTOP,
              330, 322, 100, 28, IDC_BTN_SWITCH);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_CANCEL),
              BS_PUSHBUTTON | WS_TABSTOP,
              440, 322, 100, 28, IDC_BTN_CANCEL);

    // Initial state: select the OTHER shell as the target, so a first
    // "Switch" actually changes something.
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind cur = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
    SetRadioForKind(hwnd, (cur == SHELL_EX7) ? SHELL_NATIVE : SHELL_EX7);
    RefreshStatus(hwnd);
    UpdateTargetLabel(hwnd);
    CheckDlgButton(hwnd, IDC_CHK_LOGIN,
                   LogonAutoStartPresent() ? BST_CHECKED : BST_UNCHECKED);

    SetTimer(hwnd, 1, REFRESH_TIMER_MS, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_TIMER:
        RefreshStatus(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_RADIO_NATIVE:
        case IDC_RADIO_EX7:
            if (HIWORD(wParam) == BN_CLICKED)
                UpdateTargetLabel(hwnd);
            return 0;

        case IDC_BTN_SWITCH: {
            if (MessageBoxW(hwnd, TR(TR_WARN_CONFIRM),
                    L"7explorer Shell Switcher",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            (void)DoSwitch(hwnd, SelectedTarget(hwnd), FALSE, FALSE);
            return 0;
        }

        case IDC_BTN_BROWSE:
            if (HIWORD(wParam) == BN_CLICKED)
                BrowseForExplorer7(hwnd);
            return 0;

        case IDC_BTN_THEME:
            if (HIWORD(wParam) == BN_CLICKED)
                InstallTheme(hwnd);
            return 0;

        case IDC_CHK_LOGIN:
            if (HIWORD(wParam) == BN_CLICKED) {
                BOOL want = (IsDlgButtonChecked(hwnd, IDC_CHK_LOGIN) ==
                             BST_CHECKED);
                if (want) {
                    WCHAR detail[1100];
                    DWORD taskErr = 0;
                    int rc = LogonAutoStartEnable(detail,
                                                  (DWORD)_countof(detail),
                                                  &taskErr);
                    if (rc == 2) {
                        // refused: missing explorer.exe/wrp64.dll — nothing
                        // was changed
                        WCHAR m[1400];
                        _snwprintf_s(m, _countof(m), _TRUNCATE,
                                     TR(TR_ERR_LOGON_VALIDATE_FMT), detail);
                        MessageBoxW(hwnd, m, L"7explorer Shell Switcher",
                                    MB_OK | MB_ICONERROR);
                    } else if (rc == 3) {
                        // Shell value + link OK, recovery task NOT registered
                        WCHAR m[900];
                        _snwprintf_s(m, _countof(m), _TRUNCATE,
                                     TR(TR_ERR_LOGON_TASK_FMT),
                                     (unsigned long)taskErr);
                        MessageBoxW(hwnd, m, L"7explorer Shell Switcher",
                                    MB_OK | MB_ICONWARNING);
                    } else {
                        MessageBoxW(hwnd, TR(TR_INFO_STARTUP_OK),
                                    L"7explorer Shell Switcher",
                                    MB_OK | MB_ICONINFORMATION);
                    }
                } else {
                    LogonAutoStartDisable();
                    MessageBoxW(hwnd, TR(TR_INFO_LOGON_OFF),
                                L"7explorer Shell Switcher",
                                MB_OK | MB_ICONINFORMATION);
                }
                CheckDlgButton(hwnd, IDC_CHK_LOGIN,
                               LogonAutoStartPresent() ? BST_CHECKED
                                                       : BST_UNCHECKED);
            }
            return 0;

        case IDC_BTN_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------- hotkey ---
// Emergency shortcut Ctrl+Alt+Shift+S opens this switcher even when the
// shell has crashed or hangs: a tiny resident instance (--hotkey) owns the
// hotkey and is independent of explorer. Started by the GUI and by the
// 7explorer wrapper (wrp64.dll) when found next to explorer.exe.
static const WCHAR kHotkeyMutex[] = L"Local\\7explorer.ShellSwitcher.Hotkey";

static void LaunchSelf(const WCHAR* args) {
    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    WCHAR cmd[MAX_PATH + 64];
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"%s%s", exe, args ? L" " : L"", args ? args : L"");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

static void EnsureHotkeyResident() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kHotkeyMutex);
    if (m) { CloseHandle(m); return; }
    LaunchSelf(L"--hotkey");
}

static int RunHotkeyResident() {
    HANDLE mutex = CreateMutexW(NULL, TRUE, kHotkeyMutex);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    if (!RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | 0x4000 /*MOD_NOREPEAT*/, 'S')) {
        CloseHandle(mutex); return 3;
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == 1) {
            HWND w = FindWindowW(L"Ex7ShellSwitcher", NULL);
            if (w) {
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
            } else {
                LaunchSelf(NULL);
            }
        }
    }
    UnregisterHotKey(NULL, 1);
    CloseHandle(mutex);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)nCmdShow;
    g_hInst = hInstance;

    // UI language: EX7_LANG=it|en > primary system UI language (English
    // default; the tool always works in English).
    {
        WCHAR l[8];
        DWORD n = GetEnvironmentVariableW(L"EX7_LANG", l, 7);
        if (n > 0 && n < 7 && (l[0] == L'i' || l[0] == L'I'))
            g_uiItalian = TRUE;
        else if (n > 0 && n < 7 && (l[0] == L'e' || l[0] == L'E'))
            g_uiItalian = FALSE;
        else
            g_uiItalian = (PRIMARYLANGID(GetUserDefaultUILanguage()) ==
                           LANG_ITALIAN);
    }

    ResolveShellPaths();

    // Command-line modes (used by the Startup-folder link / scripts):
    //   --apply-ex7        apply Explorer7 as shell (verified, retries for
    //                      ~60 s, restarts the hotkey resident)
    //   --logon            modifier for --apply-ex7: running from the logon
    //                      link -> fully silent, log-only errors
    //   --apply-native     apply the native shell, no confirm dialog
    //   --install-login    create the Startup-folder link ONLY (fallback
    //                      mechanism B: zero registry changes)
    //   --uninstall-login  remove the whole logon auto-start (restores the
    //                      previous Shell value, deletes link + task)
    //   --hotkey           resident Ctrl+Alt+Shift+S instance
    //   --recover-login    body of the recovery task (no UI)
    {
        int argc = 0;
        LPWSTR* argv =
            lpCmdLine && *lpCmdLine ? CommandLineToArgvW(GetCommandLineW(), &argc)
                                    : NULL;
        int mode = 0;   // 0=GUI, 1=ex7, 2=native, 3=install, 4=uninstall,
                        // 5=hotkey, 6=recover
        BOOL logonLink = FALSE;
        for (int i = 1; i < argc; i++) {
            if (!lstrcmpiW(argv[i], L"--apply-ex7"))        mode = 1;
            else if (!lstrcmpiW(argv[i], L"--apply-native"))   mode = 2;
            else if (!lstrcmpiW(argv[i], L"--install-login"))  mode = 3;
            else if (!lstrcmpiW(argv[i], L"--uninstall-login"))mode = 4;
            else if (!lstrcmpiW(argv[i], L"--hotkey"))         mode = 5;
            else if (!lstrcmpiW(argv[i], L"--recover-login"))  mode = 6;
            else if (!lstrcmpiW(argv[i], L"--logon"))          logonLink = TRUE;
            else if (!lstrcmpiW(argv[i], L"--lang=it"))        g_uiItalian = TRUE;
            else if (!lstrcmpiW(argv[i], L"--lang=en"))        g_uiItalian = FALSE;
        }
        if (argv) LocalFree(argv);

        SwLog(L"started (mode %d%s)", mode, logonLink ? L", logon link" : L"");
        if (mode == 5)
            return RunHotkeyResident();
        if (mode == 6)
            return RecoverLogin();
        if (mode == 1)
            return ApplyEx7Robust(logonLink);
        if (mode == 2) {
            int rc = DoSwitch(NULL, SHELL_NATIVE, TRUE, FALSE);
            if (rc == 0)
                EnsureHotkeyResident();  // the shortcut must survive too
            return rc;
        }
        if (mode == 3) {
            // mechanism B only: plain Startup link, zero registry changes
            DWORD e = 0;
            HRESULT hr = StartupSetPresence(TRUE, &e);
            if (FAILED(hr)) {
                WCHAR m[512];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                             L"Startup-folder link operation failed "
                             L"(error %lu).", (unsigned long)e);
                MessageBoxW(NULL, m, L"7explorer Shell Switcher",
                            MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
                return 2;
            }
            SwLog(L"--install-login: fallback link created (registry untouched)");
            return 0;
        }
        if (mode == 4) {
            // full removal, exactly like unchecking the box
            LogonAutoStartDisable();
            return 0;
        }
    }

    EnsureHotkeyResident();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Ex7ShellSwitcher";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc))
        return 1;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"Ex7ShellSwitcher",
        L"7explorer Shell Switcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 576, 400,
        NULL, NULL, hInstance, NULL);
    if (!hwnd)
        return 1;

    // Center on the work area.
    RECT wr;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wr, 0);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(hwnd, NULL,
                 wr.left + ((wr.right - wr.left) - ww) / 2,
                 wr.top + ((wr.bottom - wr.top) - wh) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {  // TAB navigation + radio keys
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
