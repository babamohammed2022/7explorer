// shell_switcher.cpp — 7explorer Shell Switcher (runtime, no logout)
//
// Small native Win32 GUI tool that switches the RUNNING shell process
// between the native Windows Explorer (%SystemRoot%\explorer.exe) and the
// private 7explorer Win7ExplorerRestorer executable (default C:\Win7ExplorerRestorerTest\explorer.exe,
// overridable via the WIN7EXPLORERRESTORER_EXPLORER_PATH environment variable).
//
// Runtime switching stays registry-free. The OPTIONAL "start at logon"
// feature (the checkbox, test37) instead sets the standard per-user shell:
//
//   - HKCU\Software\Microsoft\Windows NT\CurrentVersion\Winlogon\Shell
//     = the private explorer.exe path. This is the documented per-user way
//     to make a program the shell at logon: no elevation, current user
//     only, fully reversible. The previous value is saved and restored
//     byte-per-byte when the feature is turned off.
//   - a per-user Startup-folder link (7explorer-shell.lnk -> --apply-win7explorerestorer
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
// GetModuleFileNameW *inside* the Win7ExplorerRestorer process). No taskkill, no
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
    SHELL_WIN7EXPLORERRESTORER,        // 7explorer private explorer.exe
    SHELL_OTHER       // some explorer.exe at an unknown path
};

static WCHAR g_nativePath[1024];  // %SystemRoot%\explorer.exe
static WCHAR g_Win7ExplorerRestorerPath[1024];     // private Win7ExplorerRestorer path
static HINSTANCE g_hInst;
static HFONT g_hFont;

// Unified setup UI (test38): the window shows exactly one view at a time
// (Setup / Installing / Main). Declared here because the status helpers
// below need the current view.
enum UiView { VIEW_SETUP, VIEW_INSTALLING, VIEW_MAIN };
static UiView g_view = VIEW_SETUP;
static BOOL g_reinstall = FALSE;   // Setup shows the reinstall variant
static BOOL g_installed = FALSE;   // private copy + state\install.json seen

// ---- bilingual UI (English default; Italian for Italian systems, or
// WIN7EXPLORERRESTORER_LANG/--lang override). The switcher itself must also work in English.
typedef enum {
    TR_SUBTITLE, TR_GROUPBOX, TR_NATIVE_NAME, TR_WIN7EXPLORERRESTORER_NAME, TR_NONE_NAME,
    TR_UNKNOWN_NAME, TR_BTN_SWITCH, TR_BTN_CANCEL, TR_BTN_BROWSE,
    TR_CHK_LOGIN, TR_STATUS_CURRENT_FMT, TR_STATUS_TARGET_FMT,
    TR_WARN_CONFIRM, TR_ERR_NF_TARGET_FMT, TR_ERR_REFUSED_FMT,
    TR_INFO_ALREADY, TR_ERR_START_WIN7EXPLORERRESTORER_FMT, TR_ERR_CRITICAL_FMT,
    TR_ERR_START_NATIVE_FMT, TR_ERR_STARTUPLINK_FMT, TR_INFO_STARTUP_OK,
    TR_BROWSE_TITLE, TR_BROWSE_FILTER, TR_ERR_STARTUP_OP_FMT,
    TR_CBO_SYS, TR_CBO_EN, TR_CBO_IT,
    TR_BTN_THEME, TR_THEME_TITLE, TR_THEME_FILTER,
    TR_THEME_OK_FMT, TR_THEME_ERR_FMT,
    TR_ERR_LOGON_VALIDATE_FMT, TR_INFO_LOGON_OFF, TR_ERR_LOGON_TASK_FMT,
    // Unified setup UI (test38): every new string lives here, EN + IT.
    TR_SETUP_HEADING, TR_SETUP_DESC, TR_REINSTALL_HEADING, TR_REINSTALL_DESC,
    TR_BTN_INSTALL, TR_BTN_REINSTALL, TR_BTN_ABORT,
    TR_HINT_NOADMIN, TR_HINT_ONEMIN, TR_INST_STATUS,
    TR_INST_FAILED_FMT, TR_INST_FAILED_SHORT,
    TR_INST_ABORTED, TR_INST_MISSING_FMT, TR_INST_MISSING_SHORT,
    TR_INST_SPAWN_FMT,
    TR_MAIN_INTRO, TR_NATIVE_SUB, TR_INUSE_SUFFIX,
    TR_CHK_LOGIN_SHORT, TR_LINK_INFO, TR_LINK_REINSTALL, TR_LINK_UNINSTALL,
    TR_LANG_LABEL, TR_BTN_USE_E7, TR_BTN_USE_NATIVE, TR_HINT_RESTART,
    TR_LOGIN_HELP_TEXT, TR_REINSTALL_ASKBACK, TR_UNINSTALL_CONFIRM,
    TR_UNINSTALL_DONE,
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
    L"Select the private Win7ExplorerRestorer (explorer.exe)",
    L"explorer.exe\0explorer.exe\0All files\0*.*\0",
    L"Startup-folder link operation failed (error %lu).",
    L"System default",
    L"English",
    L"Italiano",
    L"Customize theme\u2026",
    L"Select YOUR Windows 7 theme file (aero.msstyles)",
    L"Theme files\0*.msstyles\0All files\0*.*\0",
    L"Theme installed to:\r\n%s\r\n\r\nSwitch shell (e.g. native \u2192 Win7ExplorerRestorer) to apply it.\r\n"
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
    L"Windows 7 Explorer Restorer is not installed yet",
    L"To use it, a file must be downloaded from Microsoft, verified and prepared. An Internet connection is needed only the first time.",
    L"Reinstall Windows 7 Explorer Restorer?",
    L"The private explorer.exe copy will be downloaded and prepared again. If it is in use, you will be switched back to Windows Explorer first.",
    L"Install",
    L"Reinstall",
    L"Abort",
    L"No administrator rights needed.",
    L"About a minute is needed.",
    L"Installation running\u2026",
    L"Installation failed (exit code %lu).\r\n\r\n%s",
    L"Installation failed \u2014 see details.",
    L"Installation aborted.",
    L"The installer was not found:\r\n%s\r\n\r\nCopy Win7ExplorerRestorer.exe next to this switcher (same folder) and retry.",
    L"Installer not found.",
    L"Could not start the installer (error %lu):\r\n%s",
    L"Choose which Explorer to use as the Windows shell. Switching is immediate and needs no logout.",
    L"Default system shell",
    L"  \u2013 in use",
    L"Use Windows 7 Explorer Restorer at every logon",
    L"More information",
    L"Reinstall",
    L"Uninstall",
    L"Language",
    L"Use Win7ExplorerRestorer",
    L"Use native Explorer",
    L"The desktop will restart briefly.",
    L"Automatic logon start sets the per-user Shell value (HKCU) to the private explorer.exe \u2014 the standard Windows way, no elevation, fully reversible (the previous value is saved and restored byte-for-byte).\r\n\r\nIt also adds two safety nets: a link in your Startup folder and a scheduled task (\"7explorer Shell Recovery\") that checks the shell ~30 s after logon.\r\n\r\nUncheck the box to undo everything.",
    L"Reinstallation completed.\r\n\r\nSwitch back to Windows 7 Explorer Restorer now?",
    L"Windows 7 Explorer Restorer will be removed: you will be switched back to Windows Explorer and the private files will be deleted.\r\n\r\nProceed?",
    L"Windows 7 Explorer Restorer was removed.",
};
static const WCHAR* TR_IT[TR_COUNT] = {
    L"Scambia al volo la shell Explorer attiva. Nessun logout richiesto.",
    L"Seleziona la shell Explorer",
    L"Esplora risorse Windows nativo",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(nessuna rilevata)",
    L"Explorer sconosciuto (vedi path)",
    L"Cambia", L"Annulla", L"Sfoglia\u2026",
    L"Avvia Win7ExplorerRestorer automaticamente al logon\r\n"
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
    L"Win7ExplorerRestorer verr\u00e0 avviato al logon. Applicato (tutto reversibile):\r\n"
    L"\u2022 valore Shell per-utente (HKCU ...\\Winlogon\\Shell) \u2014 valore\r\n"
    L"   precedente salvato;\r\n"
    L"\u2022 collegamento in Esecuzione automatica (fallback + riavvio hotkey);\r\n"
    L"\u2022 task di recovery \"7explorer Shell Recovery\" (~30 s dopo il logon:\r\n"
    L"   se la shell privata non \u00e8 viva ripristina tutto).\r\n"
    L"\r\n"
    L"Deseleziona la casella per annullare tutte e tre le cose (il valore\r\n"
    L"precedente viene ripristinato byte per byte). Dettagli: docs/avvio-al-login.md",
    L"Seleziona l'Win7ExplorerRestorer privato (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Tutti i file\0*.*\0",
    L"Operazione sul collegamento in Esecuzione automatica non riuscita (errore %lu).",
    L"Predefinita di sistema",
    L"English",
    L"Italiano",
    L"Personalizza tema\u2026",
    L"Seleziona il TUO file tema di Windows 7 (aero.msstyles)",
    L"File tema\0*.msstyles\0Tutti i file\0*.*\0",
    L"Tema installato in:\r\n%s\r\n\r\nCambia shell (es. nativa \u2192 Win7ExplorerRestorer) per applicarlo.\r\n"
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
    L"Windows 7 Explorer Restorer non \u00e8 ancora installato",
    L"Per usarlo occorre scaricare un file da Microsoft, verificarlo e prepararlo. Serve una connessione a Internet solo la prima volta.",
    L"Reinstallare Windows 7 Explorer Restorer?",
    L"La copia privata di explorer.exe verr\u00e0 scaricata e preparata di nuovo. Se \u00e8 in uso, si torner\u00e0 prima a Esplora risorse.",
    L"Installa",
    L"Reinstalla",
    L"Interrompi",
    L"Nessun permesso di amministratore.",
    L"L'operazione richiede circa un minuto.",
    L"Installazione in corso\u2026",
    L"Installazione non riuscita (codice %lu).\r\n\r\n%s",
    L"Installazione non riuscita \u2014 vedi dettagli.",
    L"Installazione interrotta.",
    L"Installer non trovato:\r\n%s\r\n\r\nCopia Win7ExplorerRestorer.exe nella stessa cartella dello switcher e riprova.",
    L"Installer non trovato.",
    L"Impossibile avviare l'installer (errore %lu):\r\n%s",
    L"Scegli quale Explorer usare come shell di Windows. Il cambio \u00e8 immediato e non richiede la disconnessione.",
    L"Shell predefinita del sistema",
    L"  \u2013 in uso",
    L"Usa Windows 7 Explorer Restorer a ogni accesso",
    L"Maggiori informazioni",
    L"Reinstalla",
    L"Disinstalla",
    L"Lingua",
    L"Usa Win7ExplorerRestorer",
    L"Usa Esplora risorse",
    L"Il desktop si riavvier\u00e0 brevemente.",
    L"L'avvio automatico imposta il valore Shell per-utente (HKCU) sull'explorer privato \u2014 il metodo standard di Windows, senza elevazione, completamente reversibile (il valore precedente \u00e8 salvato e ripristinato byte per byte).\r\n\r\nAggiunge anche due reti di sicurezza: un collegamento in Esecuzione automatica e un task pianificato (\"7explorer Shell Recovery\") che controlla la shell ~30 s dopo il logon.\r\n\r\nDeseleziona la casella per annullare tutto.",
    L"Reinstallazione completata.\r\n\r\nTornare ora a Windows 7 Explorer Restorer?",
    L"Windows 7 Explorer Restorer verr\u00e0 rimosso: si torner\u00e0 a Esplora risorse e i file privati verranno eliminati.\r\n\r\nProcedere?",
    L"Windows 7 Explorer Restorer rimosso.",
};
static BOOL g_uiItalian;   // FALSE = English UI (default)
static const WCHAR* TR(TRID id) { return (g_uiItalian ? TR_IT : TR_EN)[id]; }

#define IDC_RADIO_NATIVE  101
#define IDC_RADIO_WIN7EXPLORERRESTORER     102
#define IDC_BTN_SWITCH    201
#define IDC_BTN_CANCEL    202
#define IDC_BTN_BROWSE    203
#define IDC_CHK_LOGIN     204
#define IDC_CBO_SHLANG    205
#define IDC_BTN_THEME     206
#define IDC_ST_NATPATH    301
#define IDC_ST_WIN7EXPLORERRESTORERPATH    302
#define IDC_ST_CURRENT    303
#define IDC_ST_TARGET     304

#define REFRESH_TIMER_MS  1500

#define STARTUP_LINK_NAME L"7explorer-shell.lnk"

// ---- logon auto-start (test37) -------------------------------------------
// Three cooperating pieces, all per-user, all reversible (details in
// docs/avvio-al-login.md):
//   1. HKCU\...\Winlogon\Shell = private explorer.exe  (primary mechanism)
//   2. Startup-folder link -> --apply-win7explorerestorer --logon       (fallback, robust)
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
//   1. WIN7EXPLORERRESTORER_EXPLORER_PATH environment variable (expands %VAR%);
//   2. explorer.exe located NEXT TO this switcher executable (the zip bundle
//      layout: everything extracted into the same folder);
//   3. C:\Win7ExplorerRestorerTest\explorer.exe (legacy default/fallback of the bootstrap).
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
    got = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_EXPLORER_PATH", tmp,
                                  (DWORD)_countof(tmp));
    if (got > 0 && got < _countof(tmp)) {
        DWORD c = ExpandEnvironmentStringsW(tmp, g_Win7ExplorerRestorerPath,
                                            (DWORD)_countof(g_Win7ExplorerRestorerPath));
        if (c > 0 && c < _countof(g_Win7ExplorerRestorerPath))
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
                wcsncpy_s(g_Win7ExplorerRestorerPath, _countof(g_Win7ExplorerRestorerPath), tmp, _TRUNCATE);
                return;
            }
        }
    }

    // Private, 3) legacy fallback.
    wcsncpy_s(g_Win7ExplorerRestorerPath, _countof(g_Win7ExplorerRestorerPath), L"C:\\Win7ExplorerRestorerTest\\explorer.exe",
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
    if (lstrcmpiW(path, g_Win7ExplorerRestorerPath) == 0)
        return SHELL_WIN7EXPLORERRESTORER;
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
    case SHELL_WIN7EXPLORERRESTORER:    return TR(TR_WIN7EXPLORERRESTORER_NAME);
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

// Update the radio labels with the live "in use" marker (this replaces the
// old "Current shell:" status line). Skipped when nothing changed so the
// 1.5 s timer never flickers the labels.
static void RefreshStatus(HWND hwnd) {
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
    static int s_lastK = -1;
    if ((int)k == s_lastK) return;
    s_lastK = (int)k;
    HWND rN = GetDlgItem(hwnd, IDC_RADIO_NATIVE);
    HWND rE = GetDlgItem(hwnd, IDC_RADIO_WIN7EXPLORERRESTORER);
    if (!rN || !rE) return;
    WCHAR line[1200];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s%s", TR(TR_NATIVE_NAME),
                 (k == SHELL_NATIVE) ? TR(TR_INUSE_SUFFIX) : L"");
    SetWindowTextW(rN, line);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s%s",
                 TR(TR_WIN7EXPLORERRESTORER_NAME),
                 (k == SHELL_WIN7EXPLORERRESTORER) ? TR(TR_INUSE_SUFFIX) : L"");
    SetWindowTextW(rE, line);
}

// Which radio (target) the user has selected.
static ShellKind SelectedTarget(HWND hwnd) {
    if (IsDlgButtonChecked(hwnd, IDC_RADIO_WIN7EXPLORERRESTORER) == BST_CHECKED)
        return SHELL_WIN7EXPLORERRESTORER;
    return SHELL_NATIVE;
}

// The explicit main-button label ("Use ...") replaces the old "Target:"
// status line. Only meaningful on the Main view.
static void UpdateTargetLabel(HWND hwnd) {
    if (g_view != VIEW_MAIN) return;
    ShellKind t = SelectedTarget(hwnd);
    SetDlgItemTextW(hwnd, IDOK, (t == SHELL_WIN7EXPLORERRESTORER)
                    ? TR(TR_BTN_USE_E7) : TR(TR_BTN_USE_NATIVE));
}

static void SetRadioForKind(HWND hwnd, ShellKind k) {
    CheckRadioButton(hwnd, IDC_RADIO_NATIVE, IDC_RADIO_WIN7EXPLORERRESTORER,
                     (k == SHELL_WIN7EXPLORERRESTORER) ? IDC_RADIO_WIN7EXPLORERRESTORER : IDC_RADIO_NATIVE);
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
// WIN7EXPLORERRESTORER_UI_LANG in the child's inherited environment (per-process UI language
// override, honored by wrp64.dll). Returns FALSE+error on immediate failure.
static BOOL LaunchExe(LPCWSTR path, DWORD* pErr, LPCWSTR envLang) {
    WCHAR cmd[1200];
    wcsncpy_s(cmd, _countof(cmd), path, _TRUNCATE);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // Temporarily export WIN7EXPLORERRESTORER_UI_LANG for the child, then restore. The child
    // receives a COPY of the environment at CreateProcess time.
    WCHAR prev[32];
    DWORD had = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", prev, 30);
    if (envLang)
        SetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", envLang);
    BOOL ok = CreateProcessW(path, cmd, NULL, NULL, FALSE,
                             CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi);
    if (envLang) {
        if (had > 0 && had < 30)
            SetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", prev);
        else
            SetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", NULL);
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

// UI-language selection for the Win7ExplorerRestorer launch: from the GUI combo (0/1/2) or
// from the caller environment (headless CLI inherits WIN7EXPLORERRESTORER_UI_LANG as-is).
// This is the language-application point (shell-only, via the child env in
// LaunchExe): it never throws out; on any failure fall back to NULL
// (system default for OUR shell; the OS language is never touched).
static LPCWSTR SelectedShellUILang(HWND hwnd) {
    try {
        if (!hwnd) {
            WCHAR cur[32];
            DWORD n = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", cur, 31);
            return (n > 0 && n < 31) ? L"ENV" : NULL;  // handled inside LaunchExe
        }
        int sel = (int)SendMessageW(GetDlgItem(hwnd, IDC_CBO_SHLANG),
                                    CB_GETCURSEL, 0, 0);
        if (sel == 1) return L"en-US";
        if (sel == 2) return L"it-IT";
        return NULL;  // system default
    } catch (...) {
        SwLog(L"shell-lang: selection read failed, using system default");
        return NULL;
    }
}

// ---------------------------------------------------------- switching ---

// headless: TRUE when invoked from the command line (--apply-*); suppresses
// informational popups, keeps error popups (message boxes work with hwnd
// NULL). silent: additionally suppresses ALL popups (background/logon use —
// errors are reported via the log and the exit code only).
// Returns 0 on success, 2 on failure (used as process exit code).
static int DoSwitch(HWND hwnd, ShellKind target, BOOL headless, BOOL silent) {
    const WCHAR* targetPath = (target == SHELL_WIN7EXPLORERRESTORER) ? g_Win7ExplorerRestorerPath
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
    LPCWSTR envLang = (target == SHELL_WIN7EXPLORERRESTORER) ? SelectedShellUILang(hwnd) : NULL;
    if (envLang && lstrcmpiW(envLang, L"ENV") == 0)
        envLang = NULL;  // headless: keep caller environment as-is
    if (!LaunchExe(targetPath, &err, envLang)) {
        WCHAR msg[1500];
        failed = 1;
        SwLog(L"switch: CreateProcess(%s) failed with error %lu",
              targetPath, (unsigned long)err);
        if (target != SHELL_NATIVE && FileExists(g_nativePath)) {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         TR(TR_ERR_START_WIN7EXPLORERRESTORER_FMT),
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
// at this tool with --apply-win7explorerestorer --logon (robust retry + verification +
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
    //          "<own exe>" --apply-win7explorerestorer
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
            sl->SetArguments(L"--apply-win7explorerestorer --logon");
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
    if (!FileExists(g_Win7ExplorerRestorerPath)) {
        _snwprintf_s(pErr, errChars, _TRUNCATE, L"%s", g_Win7ExplorerRestorerPath);
        return FALSE;
    }
    WCHAR wrp[1100];
    wcsncpy_s(wrp, _countof(wrp), g_Win7ExplorerRestorerPath, _TRUNCATE);
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
    return WriteShellValue(g_Win7ExplorerRestorerPath, written, (DWORD)_countof(written));
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
        if (FAILED(hr = svc->NewTask(0, &def))) break;  // ITaskService::NewTask
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

// --------------------------------------- robust logon --apply-win7explorerestorer -------
// Used by the Startup-folder link (--apply-win7explorerestorer --logon) and by plain
// --apply-win7explorerestorer. The switch is attempted and then VERIFIED (the shell window
// owner must really be the private explorer); on failure it retries with
// backoff for about a minute. On success the --hotkey resident is (re)
// started. kApplyRunningMutex tells --recover-login that a switch is still
// being attempted, so the recovery task does not fight it.

static BOOL ApplyMutexHold(HANDLE* pMutex) {
    HANDLE m = CreateMutexW(NULL, TRUE, kApplyRunningMutex);
    *pMutex = m;
    return m != NULL && GetLastError() != ERROR_ALREADY_EXISTS;
}

static int ApplyWin7ExplorerRestorerRobust(BOOL fromLogonLink) {
    static const DWORD waitMs[] = { 5000, 10000, 15000, 20000, 25000 };
    const DWORD totalMs = 60000;
    DWORD spent = 0;
    int rc = 2;
    SwLog(L"apply-win7explorerestorer (%s): target %s",
          fromLogonLink ? L"logon link" : L"manual",
          g_Win7ExplorerRestorerPath);
    HANDLE mutex = NULL;
    BOOL ownMutex = fromLogonLink && ApplyMutexHold(&mutex);
    for (int attempt = 1; ; attempt++) {
        WCHAR path[1024];
        DWORD pid = 0;
        ShellKind k = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
        if (k == SHELL_WIN7EXPLORERRESTORER && pid != 0) {
            SwLog(L"apply-win7explorerestorer: verified, shell pid %u (%s)", pid, path);
            rc = 0;
            break;
        }
        if (!FileExists(g_Win7ExplorerRestorerPath)) {
            SwLog(L"apply-win7explorerestorer: target missing, not retrying");
            rc = 2;
            break;
        }
        if (attempt > 1)
            SwLog(L"apply-win7explorerestorer: attempt %d (shell is %s), retrying",
                  attempt, KindName(k));
        rc = DoSwitch(NULL, SHELL_WIN7EXPLORERRESTORER, TRUE, fromLogonLink /*silent*/);
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
        SwLog(L"apply-win7explorerestorer: giving up after ~%lu ms (rc=%d)",
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
        if (k == SHELL_WIN7EXPLORERRESTORER && pid != 0) {
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
// copy of their own Windows 7) and copies it next to the private Win7ExplorerRestorer
// as <Win7ExplorerRestorerDir>\theme\aero.msstyles, the exact layout the wrapper's
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

    // destination: <dir of the private Win7ExplorerRestorer>\theme\aero.msstyles
    WCHAR dst[1024];
    wcsncpy_s(dst, _countof(dst), g_Win7ExplorerRestorerPath, _TRUNCATE);
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

static void BrowseForWin7ExplorerRestorer(HWND hwnd) {
    WCHAR file[1024];
    wcsncpy_s(file, _countof(file), g_Win7ExplorerRestorerPath, _TRUNCATE);
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
        wcsncpy_s(g_Win7ExplorerRestorerPath, _countof(g_Win7ExplorerRestorerPath), file, _TRUNCATE);
        SetDlgItemTextW(hwnd, IDC_ST_WIN7EXPLORERRESTORERPATH, g_Win7ExplorerRestorerPath);
        UpdateTargetLabel(hwnd);
    }
}

// ==================================== unified setup UI (test38) ===
// Two-view window modeled on the Windhawk prototype
// "shell-switcher-ui-test.wh.cpp": a Setup view (installer launcher +
// progress) and the Main view (shell choice + logon + language/theme).
// Only UI + new flows live here; DoSwitch/logon/recovery logic above is
// untouched. No new external dependencies (user32/gdi32/comctl32/comdlg32
// were already linked).

#define IDC_LINK_INFO       207
#define IDC_LINK_REINSTALL  208
#define IDC_LINK_UNINSTALL  209
#define IDC_ST_NATSUB       305
#define IDC_ST_LANG         306

#define TID_REFRESH  1
#define TID_INSTALL  2
#define INSTALL_TIMER_MS 500

static void ApplyView(HWND hwnd);  // defined with the window below

// ---- tiny RAII guards (C++17, no STL; same conservative style) ---------
struct RegKeyGuard {
    HKEY h;
    RegKeyGuard() : h(NULL) {}
    ~RegKeyGuard() { Close(); }
    void Close() { if (h) { RegCloseKey(h); h = NULL; } }
    HKEY* Put() { Close(); return &h; }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
};

struct ProcHandle {
    HANDLE h;
    ProcHandle() : h(NULL) {}
    ~ProcHandle() { Close(); }
    void Close() { if (h && h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = NULL; } }
    ProcHandle(const ProcHandle&) = delete;
    ProcHandle& operator=(const ProcHandle&) = delete;
};

// ---- DPI scaling --------------------------------------------------------
// No manifest: the process is DPI-virtualized by default. On the GUI path
// wWinMain calls SetProcessDPIAware() (user32, Vista+) and every layout
// coordinate below goes through Dpx(), so the window stays crisp at
// 125%/150%/200% even though it is laid out in 96-DPI units.
static int g_dpi = 96;
static void DpiInit(void) {
    HDC dc = GetDC(NULL);
    if (dc) {
        int v = GetDeviceCaps(dc, LOGPIXELSX);
        if (v >= 96 && v <= 480) g_dpi = v;
        ReleaseDC(NULL, dc);
    }
}
static int Dpx(int px) { return MulDiv(px, g_dpi, 96); }

// ---- UI fonts (system message font + bold + link) -----------------------
static HFONT g_hFontBold = NULL;
static HFONT g_hFontLink = NULL;

static void CreateUiFonts(void) {
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return;  // keep g_hFont as-is (NULL -> system default rendering)
    if (g_hFont) DeleteObject(g_hFont);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    if (g_hFontBold) DeleteObject(g_hFontBold);
    g_hFontBold = CreateFontIndirectW(&bold);
    LOGFONTW link = ncm.lfMessageFont;
    link.lfUnderline = TRUE;
    if (g_hFontLink) DeleteObject(g_hFontLink);
    g_hFontLink = CreateFontIndirectW(&link);
}
static void DestroyUiFonts(void) {
    if (g_hFont) DeleteObject(g_hFont);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    if (g_hFontLink) DeleteObject(g_hFontLink);
    g_hFont = NULL;
    g_hFontBold = NULL;
    g_hFontLink = NULL;
}

// ---- view state + control panels ----------------------------------------
static HWND g_setupCtrls[8];
static int g_nSetup = 0;
static HWND g_mainCtrls[20];
static int g_nMain = 0;
static HWND* g_panelDst = NULL;  // append target for MakeChild (or NULL)
static int* g_panelCnt = NULL;
static int g_panelCap = 0;

static HWND g_heading = NULL;
static HWND g_desc = NULL;
static HWND g_progress = NULL;
static HWND g_status = NULL;
static HWND g_hint = NULL;
static HWND g_natSub = NULL;
static int g_marquee = 0;

// ---- bundle paths -------------------------------------------------------
static void SwitcherDir(LPWSTR out, DWORD cch) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcsncpy_s(out, cch, (n > 0 && n < MAX_PATH) ? exe : L".", _TRUNCATE);
    WCHAR* bs = wcsrchr(out, L'\\');
    if (bs) *bs = L'\0';
}

static void Join2(LPWSTR out, DWORD cch, LPCWSTR dir, LPCWSTR rest) {
    _snwprintf_s(out, cch, _TRUNCATE, L"%s\\%s", dir, rest);
}

static void InstallerExePath(LPWSTR out, DWORD cch) {
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    Join2(out, cch, dir, L"Win7ExplorerRestorer.exe");
}
static void InstallLogPath(LPWSTR out, DWORD cch) {
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    Join2(out, cch, dir, L"log\\Win7ExplorerRestorerSetup.log");
}
static void SideExplorerPath(LPWSTR out, DWORD cch) {
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    Join2(out, cch, dir, L"explorer.exe");
}

// Installed = the installer completed here: side-by-side explorer.exe (or
// the ResolveShellPaths override/Browse path) AND state\install.json.
static BOOL IsInstalled(void) {
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    WCHAR json[MAX_PATH]; Join2(json, MAX_PATH, dir, L"state\\install.json");
    if (!FileExists(json)) return FALSE;
    WCHAR side[MAX_PATH]; Join2(side, MAX_PATH, dir, L"explorer.exe");
    return FileExists(side) || FileExists(g_Win7ExplorerRestorerPath);
}

static void SetStatus(LPCWSTR text) {
    if (g_status) SetWindowTextW(g_status, text);
}

// ---- shell UI language: OUR SHELL ONLY ----------------------------------
// The combo selects the UI language of the PRIVATE explorer only. It is
// persisted per-user under OUR OWN HKCU key and applied EXCLUSIVELY by
// passing WIN7EXPLORERRESTORER_UI_LANG in the child process environment at
// CreateProcess time (see LaunchExe: the child gets a COPY). This NEVER
// touches system-wide language state: no SetThreadUILanguage /
// SetProcessPreferredUILanguages, no HKLM, no MUI registry values.
// Handles are RAII-guarded and every step is wrapped in try/catch with a
// safe fallback (system default), as required.
static const WCHAR kSwitcherRegKey[] = L"Software\\7explorer\\ShellSwitcher";
static const WCHAR kShellLangValue[] = L"ShellUILang";  // REG_DWORD 0/1/2

static int ShellLangLoad(void) {
    try {
        RegKeyGuard k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSwitcherRegKey, 0,
                          KEY_READ, k.Put()) != ERROR_SUCCESS)
            return 0;
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (RegQueryValueExW(k.h, kShellLangValue, NULL, &type,
                             (LPBYTE)&v, &cb) != ERROR_SUCCESS ||
            type != REG_DWORD || v > 2)
            return 0;
        return (int)v;
    } catch (...) {
        SwLog(L"shell-lang: load failed, using system default");
        return 0;
    }
}

static void ShellLangSave(int sel) {
    if (sel < 0 || sel > 2) sel = 0;
    try {
        RegKeyGuard k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSwitcherRegKey, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, k.Put(),
                            NULL) != ERROR_SUCCESS)
            return;
        DWORD v = (DWORD)sel;
        RegSetValueExW(k.h, kShellLangValue, 0, REG_DWORD,
                       (const BYTE*)&v, sizeof(v));
        SwLog(L"shell-lang: saved selection %d (HKCU, our shell only)", sel);
    } catch (...) {
        SwLog(L"shell-lang: save failed (kept for this session only)");
    }
}

// ---- installer child process --------------------------------------------
// Runs <switcher-dir>\Win7ExplorerRestorer.exe hidden and polls it with a
// timer; the UI never blocks. No installer change was needed: cancel is
// TerminateProcess (as specified) plus a snapshot check that deletes a
// half-written private explorer.exe, so the state stays coherent.
static ProcHandle g_installProc;
static BOOL g_preSnapValid = FALSE;
static DWORD g_preSizeLow = 0, g_preSizeHigh = 0;
static FILETIME g_preWrite = { 0, 0 };

// Snapshot of the side-by-side target before launching the installer, so
// an abort/failure can tell "installer never touched it" (keep it) from
// "half-written" (delete it). Never touches the native executable.
static void SnapshotTarget(void) {
    WCHAR t[MAX_PATH]; SideExplorerPath(t, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(t, GetFileExInfoStandard, &d) &&
        !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        g_preSnapValid = TRUE;
        g_preSizeLow = d.nFileSizeLow;
        g_preSizeHigh = d.nFileSizeHigh;
        g_preWrite = d.ftLastWriteTime;
    } else {
        g_preSnapValid = FALSE;
    }
}

static BOOL IsNativePath(LPCWSTR p) {
    return lstrcmpiW(p, g_nativePath) == 0;
}

static void MaybeCleanupSuspectTarget(void) {
    WCHAR t[MAX_PATH]; SideExplorerPath(t, MAX_PATH);
    if (IsNativePath(t)) {
        SwLog(L"cleanup: refusing to touch the native path");
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL exists = GetFileAttributesExW(t, GetFileExInfoStandard, &d) &&
                  !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!g_preSnapValid) {
        // No file before: anything there now is the installer's partial
        // output -> remove it so a retry starts clean.
        if (exists) {
            SwLog(L"cleanup: deleting partial %s", t);
            DeleteFileW(t);
        }
        return;
    }
    if (!exists) return;  // installer removed it; nothing to do
    if (d.nFileSizeLow != g_preSizeLow || d.nFileSizeHigh != g_preSizeHigh ||
        CompareFileTime(&d.ftLastWriteTime, &g_preWrite) != 0) {
        SwLog(L"cleanup: target changed mid-run, deleting suspect %s", t);
        DeleteFileW(t);
    } else {
        SwLog(L"cleanup: target untouched, keeping it");
    }
}

static BOOL StartInstall(HWND hwnd) {
    if (g_installProc.h) return FALSE;  // already running
    WCHAR exe[MAX_PATH]; InstallerExePath(exe, MAX_PATH);
    if (!FileExists(exe)) {
        WCHAR m[1400];
        _snwprintf_s(m, _countof(m), _TRUNCATE, TR(TR_INST_MISSING_FMT), exe);
        SwLog(L"install: installer missing: %s", exe);
        SetStatus(TR(TR_INST_MISSING_SHORT));
        MessageBoxW(hwnd, m, L"7explorer Shell Switcher", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    SnapshotTarget();
    WCHAR cmd[MAX_PATH + 16];
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"", exe);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // CREATE_NO_WINDOW: the installer is a console tool; keep it hidden.
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        SwLog(L"install: CreateProcess failed (%lu)", e);
        WCHAR m[1200];
        _snwprintf_s(m, _countof(m), _TRUNCATE, TR(TR_INST_SPAWN_FMT), e, exe);
        SetStatus(TR(TR_INST_FAILED_SHORT));
        MessageBoxW(hwnd, m, L"7explorer Shell Switcher", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    g_installProc.h = pi.hProcess;
    g_marquee = 0;
    g_view = VIEW_INSTALLING;
    ApplyView(hwnd);
    SetStatus(TR(TR_INST_STATUS));
    SetTimer(hwnd, TID_INSTALL, INSTALL_TIMER_MS, NULL);
    SwLog(L"install: launched %s (pid %u)", exe, (unsigned)pi.dwProcessId);
    return TRUE;
}

// Last ~8 non-empty lines of the installer's UTF-16 log. Shared read: the
// installer log is opened FILE_SHARE_READ, so this works even mid-run.
static void ReadInstallerLogTail(LPWSTR out, DWORD cch) {
    out[0] = L'\0';
    if (cch < 16) return;
    WCHAR log[MAX_PATH]; InstallLogPath(log, MAX_PATH);
    HANDLE h = CreateFileW(log, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static WCHAR buf[4098];  // 8 KB window at the tail
    DWORD rd = 0;
    LONGLONG chunkOff = 0;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        LONGLONG bytes = sz.QuadPart > 8192 ? 8192 : sz.QuadPart;
        bytes &= ~1LL;  // keep WCHAR alignment (log is UTF-16)
        chunkOff = sz.QuadPart - bytes;
        LARGE_INTEGER off;
        off.QuadPart = chunkOff;
        if (SetFilePointerEx(h, off, NULL, FILE_BEGIN) &&
            ReadFile(h, buf, (DWORD)bytes, &rd, NULL))
            buf[rd / 2] = L'\0';
        else
            rd = 0;
    }
    CloseHandle(h);
    if (rd < 2) return;
    // Split into lines (in place), then keep the last 8 non-empty ones.
    WCHAR* lines[64];
    int n = 0;
    WCHAR* cur = buf;
    if (chunkOff > 0) {
        while (*cur && *cur != L'\r' && *cur != L'\n') cur++;  // partial 1st line
    } else if (*cur == 0xFEFF) {
        cur++;  // BOM
    }
    while (*cur && n < 64) {
        while (*cur == L'\r' || *cur == L'\n') cur++;
        if (!*cur) break;
        lines[n++] = cur;
        while (*cur && *cur != L'\r' && *cur != L'\n') cur++;
        if (*cur) *cur++ = L'\0';
    }
    int first = n > 8 ? n - 8 : 0;
    size_t used = 0;
    for (int i = first; i < n; i++) {
        size_t len = wcslen(lines[i]);
        while (len > 0 && (lines[i][len - 1] == L' ' || lines[i][len - 1] == L'\t'))
            lines[i][--len] = L'\0';
        if (len == 0) continue;
        if (used > 0 && used + 2 < cch) {
            out[used++] = L'\r';
            out[used++] = L'\n';
        }
        for (size_t k = 0; k < len && used + 1 < cch; k++)
            out[used++] = lines[i][k];
    }
    out[used] = L'\0';
}

static void FinishInstall(HWND hwnd, DWORD exitCode) {
    WCHAR tail[1600]; ReadInstallerLogTail(tail, _countof(tail));
    SwLog(L"install: child exited with code %lu", exitCode);
    if (exitCode != 0) MaybeCleanupSuspectTarget();
    ResolveShellPaths();
    g_installed = IsInstalled();
    if (exitCode == 0 && g_installed) {
        if (g_progress) SendMessageW(g_progress, PBM_SETPOS, 100, 0);
        SwLog(L"install: success");
        BOOL back = g_reinstall;
        g_reinstall = FALSE;
        g_view = VIEW_MAIN;
        ApplyView(hwnd);
        RefreshStatus(hwnd);
        if (back &&
            MessageBoxW(hwnd, TR(TR_REINSTALL_ASKBACK),
                        L"7explorer Shell Switcher",
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            (void)DoSwitch(hwnd, SHELL_WIN7EXPLORERRESTORER, FALSE, FALSE);
            ApplyView(hwnd);
        }
        return;
    }
    // Failure: stay on Setup with the log tail as the message.
    g_view = VIEW_SETUP;
    ApplyView(hwnd);
    WCHAR m[2200];
    _snwprintf_s(m, _countof(m), _TRUNCATE, TR(TR_INST_FAILED_FMT),
                 exitCode, tail[0] ? tail : L"-");
    SetStatus(TR(TR_INST_FAILED_SHORT));
    MessageBoxW(hwnd, m, L"7explorer Shell Switcher", MB_OK | MB_ICONERROR);
}

static void PollInstaller(HWND hwnd) {
    if (!g_installProc.h) {
        KillTimer(hwnd, TID_INSTALL);
        return;
    }
    if (WaitForSingleObject(g_installProc.h, 0) != WAIT_OBJECT_0) {
        // Alive: indeterminate animation (no real progress data exists).
        g_marquee = (g_marquee + 11) % 111;
        int pos = g_marquee > 100 ? 100 - (g_marquee - 100) : g_marquee;
        if (g_progress) SendMessageW(g_progress, PBM_SETPOS, pos, 0);
        return;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(g_installProc.h, &exitCode);
    g_installProc.Close();
    KillTimer(hwnd, TID_INSTALL);
    FinishInstall(hwnd, exitCode);
}

static void AbortInstall(HWND hwnd, BOOL silent) {
    if (g_installProc.h) {
        SwLog(L"install: aborting child process");
        TerminateProcess(g_installProc.h, 1);
        WaitForSingleObject(g_installProc.h, 3000);
        g_installProc.Close();
        MaybeCleanupSuspectTarget();
    }
    KillTimer(hwnd, TID_INSTALL);
    ResolveShellPaths();
    g_installed = IsInstalled();
    g_view = VIEW_SETUP;  // keep g_reinstall: retry keeps the variant
    ApplyView(hwnd);
    SetStatus(TR(TR_INST_ABORTED));
    if (!silent) SwLog(L"install: aborted by user");
}

// ---- reinstall: native first, install, optional switch back -------------
static void EnterReinstall(HWND hwnd) {
    WCHAR cur[1024]; DWORD pid = 0;
    if (DetectCurrentShell(cur, _countof(cur), &pid) == SHELL_WIN7EXPLORERRESTORER) {
        // Our shell is live: move to native FIRST (DoSwitch refuses
        // anything unsafe by itself), then install into the quiet files.
        SwLog(L"reinstall: switching to native first");
        if (DoSwitch(hwnd, SHELL_NATIVE, FALSE, FALSE) != 0) {
            SwLog(L"reinstall: pre-switch failed, staying on Main");
            return;
        }
    }
    g_reinstall = TRUE;
    g_view = VIEW_SETUP;
    ApplyView(hwnd);
    SetStatus(L"");
}

// ---- uninstall: native shell, logon off, private files gone -------------
// Only our own files under the switcher folder. NEVER the native
// C:\Windows\explorer.exe and NEVER HKLM (untouched here by design).
static BOOL IsUnderDir(LPCWSTR dir, LPCWSTR path) {
    size_t n = wcslen(dir);
    return _wcsnicmp(path, dir, n) == 0 &&
           (path[n] == L'\\' || path[n] == L'\0');
}

static void DeleteDirTree(LPCWSTR dir) {
    WCHAR spec[MAX_PATH];
    _snwprintf_s(spec, _countof(spec), _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(spec, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!lstrcmpW(f.cFileName, L".") || !lstrcmpW(f.cFileName, L".."))
            continue;
        WCHAR p[MAX_PATH];
        _snwprintf_s(p, _countof(p), _TRUNCATE, L"%s\\%s", dir, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirTree(p);
            RemoveDirectoryW(p);
        } else {
            SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p);
        }
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

static void DoUninstall(HWND hwnd) {
    if (MessageBoxW(hwnd, TR(TR_UNINSTALL_CONFIRM),
                    L"7explorer Shell Switcher",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    WCHAR cur[1024]; DWORD pid = 0;
    if (DetectCurrentShell(cur, _countof(cur), &pid) == SHELL_WIN7EXPLORERRESTORER) {
        SwLog(L"uninstall: switching to native first");
        if (DoSwitch(hwnd, SHELL_NATIVE, FALSE, FALSE) != 0) {
            SwLog(L"uninstall: pre-switch failed, nothing removed");
            return;
        }
    }
    LogonAutoStartDisable();
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    WCHAR t[MAX_PATH]; SideExplorerPath(t, MAX_PATH);
    if (!IsNativePath(t) && IsUnderDir(dir, t)) {
        SwLog(L"uninstall: deleting %s", t);
        SetFileAttributesW(t, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(t);
    } else {
        SwLog(L"uninstall: REFUSED to delete %s (safety guard)", t);
    }
    WCHAR sub[MAX_PATH];
    Join2(sub, MAX_PATH, dir, L"cache");
    if (IsUnderDir(dir, sub)) {
        DeleteDirTree(sub);
        RemoveDirectoryW(sub);
    }
    Join2(sub, MAX_PATH, dir, L"state");
    if (IsUnderDir(dir, sub)) {
        DeleteDirTree(sub);
        RemoveDirectoryW(sub);
    }
    SwLog(L"uninstall: private explorer.exe + cache + state removed");
    g_installed = FALSE;
    g_reinstall = FALSE;
    ResolveShellPaths();
    g_view = VIEW_SETUP;
    ApplyView(hwnd);
    SetStatus(TR(TR_UNINSTALL_DONE));
}

// ---- "More information": shipped doc, else built-in equivalent -----------
// The label never names a file; the doc lookup is an implementation detail.
static void OpenLogonHelp(HWND hwnd) {
    WCHAR dir[MAX_PATH]; SwitcherDir(dir, MAX_PATH);
    WCHAR cand[MAX_PATH];
    const WCHAR* subs[] = { L"docs\\avvio-al-login.md",
                            L"avvio-al-login.md" };
    for (int i = 0; i < 2; i++) {
        Join2(cand, MAX_PATH, dir, subs[i]);
        if (FileExists(cand)) {
            SwLog(L"help: opening %s", cand);
            HINSTANCE r = ShellExecuteW(hwnd, L"open", cand, NULL, NULL,
                                        SW_SHOWNORMAL);
            if ((INT_PTR)r > 32) return;
            break;  // exists but no viewer: fall through to built-in text
        }
    }
    // Repo/dev layout: <repo>\docs next to the <repo>\switcher folder.
    WCHAR parent[MAX_PATH];
    wcsncpy_s(parent, _countof(parent), dir, _TRUNCATE);
    WCHAR* bs = wcsrchr(parent, L'\\');
    if (bs) {
        *bs = L'\0';
        Join2(cand, MAX_PATH, parent, L"docs\\avvio-al-login.md");
        if (FileExists(cand)) {
            HINSTANCE r = ShellExecuteW(hwnd, L"open", cand, NULL, NULL,
                                        SW_SHOWNORMAL);
            if ((INT_PTR)r > 32) return;
        }
    }
    MessageBoxW(hwnd, TR(TR_LOGIN_HELP_TEXT), TR(TR_LINK_INFO),
                MB_OK | MB_ICONINFORMATION);
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
    if (c && g_panelDst && g_panelCnt && *g_panelCnt < g_panelCap)
        g_panelDst[(*g_panelCnt)++] = c;  // panel tracking for ApplyView
    return c;
}

// ---- panels --------------------------------------------------------------
// 96-DPI layout (every coordinate goes through Dpx()); client area 480x330,
// modeled on the prototype. Deviation: progress/status sit 8px lower so the
// 4-line description box never clips; the window is NOT topmost (the
// prototype flag was test-only) and keeps its minimize box.
static void BuildSetupPanel(HWND hwnd) {
    g_panelDst = g_setupCtrls;
    g_panelCnt = &g_nSetup;
    g_panelCap = (int)_countof(g_setupCtrls);
    g_nSetup = 0;
    g_heading = MakeChild(hwnd, WC_STATICW, L"", 0,
                          Dpx(16), Dpx(16), Dpx(448), Dpx(20), 0);
    if (g_heading && g_hFontBold)
        SendMessageW(g_heading, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    g_desc = MakeChild(hwnd, WC_STATICW, L"", 0,
                       Dpx(16), Dpx(44), Dpx(448), Dpx(64), 0);
    g_progress = MakeChild(hwnd, PROGRESS_CLASSW, L"", PBS_SMOOTH,
                           Dpx(16), Dpx(122), Dpx(448), Dpx(18), 0);
    if (g_progress) SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
    g_status = MakeChild(hwnd, WC_STATICW, L"", 0,
                         Dpx(16), Dpx(148), Dpx(448), Dpx(18), 0);
    g_panelDst = NULL;
    g_panelCnt = NULL;
}

static void BuildMainPanel(HWND hwnd) {
    const DWORD tab = WS_TABSTOP;
    g_panelDst = g_mainCtrls;
    g_panelCnt = &g_nMain;
    g_panelCap = (int)_countof(g_mainCtrls);
    g_nMain = 0;
    MakeChild(hwnd, WC_STATICW, TR(TR_MAIN_INTRO), 0,
              Dpx(16), Dpx(14), Dpx(448), Dpx(34), 0);
    HWND rN = MakeChild(hwnd, WC_BUTTONW, TR(TR_NATIVE_NAME),
                        BS_AUTORADIOBUTTON | WS_GROUP | tab,
                        Dpx(16), Dpx(58), Dpx(448), Dpx(20), IDC_RADIO_NATIVE);
    if (rN && g_hFontBold)
        SendMessageW(rN, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    g_natSub = MakeChild(hwnd, WC_STATICW, TR(TR_NATIVE_SUB), 0,
                         Dpx(38), Dpx(78), Dpx(420), Dpx(16), IDC_ST_NATSUB);
    HWND rE = MakeChild(hwnd, WC_BUTTONW, TR(TR_WIN7EXPLORERRESTORER_NAME),
                        BS_AUTORADIOBUTTON | tab,
                        Dpx(16), Dpx(104), Dpx(448), Dpx(20),
                        IDC_RADIO_WIN7EXPLORERRESTORER);
    if (rE && g_hFontBold)
        SendMessageW(rE, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
    MakeChild(hwnd, WC_STATICW, g_Win7ExplorerRestorerPath, SS_PATHELLIPSIS,
              Dpx(38), Dpx(126), Dpx(322), Dpx(16),
              IDC_ST_WIN7EXPLORERRESTORERPATH);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_BROWSE), BS_PUSHBUTTON | tab,
              Dpx(372), Dpx(122), Dpx(92), Dpx(24), IDC_BTN_BROWSE);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_CHK_LOGIN_SHORT),
              BS_AUTOCHECKBOX | tab,
              Dpx(16), Dpx(158), Dpx(448), Dpx(20), IDC_CHK_LOGIN);
    HWND lnk = MakeChild(hwnd, WC_STATICW, TR(TR_LINK_INFO),
                         SS_NOTIFY | tab,
                         Dpx(38), Dpx(180), Dpx(150), Dpx(16), IDC_LINK_INFO);
    if (lnk && g_hFontLink)
        SendMessageW(lnk, WM_SETFONT, (WPARAM)g_hFontLink, TRUE);
    lnk = MakeChild(hwnd, WC_STATICW, TR(TR_LINK_REINSTALL),
                    SS_NOTIFY | tab,
                    Dpx(296), Dpx(180), Dpx(70), Dpx(16), IDC_LINK_REINSTALL);
    if (lnk && g_hFontLink)
        SendMessageW(lnk, WM_SETFONT, (WPARAM)g_hFontLink, TRUE);
    lnk = MakeChild(hwnd, WC_STATICW, TR(TR_LINK_UNINSTALL),
                    SS_NOTIFY | tab,
                    Dpx(384), Dpx(180), Dpx(80), Dpx(16), IDC_LINK_UNINSTALL);
    if (lnk && g_hFontLink)
        SendMessageW(lnk, WM_SETFONT, (WPARAM)g_hFontLink, TRUE);
    MakeChild(hwnd, WC_STATICW, L"", SS_ETCHEDHORZ,
              Dpx(16), Dpx(206), Dpx(448), Dpx(2), 0);
    MakeChild(hwnd, WC_STATICW, TR(TR_LANG_LABEL), 0,
              Dpx(16), Dpx(222), Dpx(44), Dpx(18), IDC_ST_LANG);
    HWND cbo = MakeChild(hwnd, WC_COMBOBOXW, NULL,
                         CBS_DROPDOWNLIST | WS_VSCROLL | tab,
                         Dpx(62), Dpx(218), Dpx(190), Dpx(120), IDC_CBO_SHLANG);
    if (cbo) {
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_SYS));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_EN));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_IT));
        SendMessageW(cbo, CB_SETCURSEL, ShellLangLoad(), 0);
    }
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_THEME), BS_PUSHBUTTON | tab,
              Dpx(318), Dpx(217), Dpx(146), Dpx(24), IDC_BTN_THEME);
    g_panelDst = NULL;
    g_panelCnt = NULL;
}

static void BuildFooter(HWND hwnd) {
    const DWORD tab = WS_TABSTOP;
    MakeChild(hwnd, WC_STATICW, L"", SS_ETCHEDHORZ,
              Dpx(0), Dpx(272), Dpx(480), Dpx(2), 0);
    g_hint = MakeChild(hwnd, WC_STATICW, L"", 0,
                       Dpx(16), Dpx(290), Dpx(154), Dpx(32), 0);
    // IDOK/IDCANCEL: Enter/Esc behave like a normal dialog.
    MakeChild(hwnd, WC_BUTTONW, L"", BS_DEFPUSHBUTTON | tab,
              Dpx(178), Dpx(286), Dpx(176), Dpx(26), IDOK);
    MakeChild(hwnd, WC_BUTTONW, TR(TR_BTN_CANCEL), BS_PUSHBUTTON | tab,
              Dpx(362), Dpx(286), Dpx(102), Dpx(26), IDCANCEL);
}

static BOOL IsLinkId(int id) {
    return id == IDC_LINK_INFO || id == IDC_LINK_REINSTALL ||
           id == IDC_LINK_UNINSTALL;
}

static void ApplyView(HWND hwnd) {
    const BOOL setup = (g_view != VIEW_MAIN);
    for (int i = 0; i < g_nSetup; i++)
        ShowWindow(g_setupCtrls[i], setup ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < g_nMain; i++)
        ShowWindow(g_mainCtrls[i], setup ? SW_HIDE : SW_SHOW);
    if (setup) {
        const BOOL running = (g_view == VIEW_INSTALLING);
        if (g_progress) ShowWindow(g_progress, running ? SW_SHOW : SW_HIDE);
        if (g_heading)
            SetWindowTextW(g_heading, g_reinstall ? TR(TR_REINSTALL_HEADING)
                                                  : TR(TR_SETUP_HEADING));
        if (g_desc)
            SetWindowTextW(g_desc, g_reinstall ? TR(TR_REINSTALL_DESC)
                                               : TR(TR_SETUP_DESC));
        if (!running) SetStatus(L"");
        SetDlgItemTextW(hwnd, IDOK, running ? TR(TR_BTN_ABORT)
                               : (g_reinstall ? TR(TR_BTN_REINSTALL)
                                              : TR(TR_BTN_INSTALL)));
        if (g_hint)
            SetWindowTextW(g_hint, running ? TR(TR_HINT_ONEMIN)
                                           : TR(TR_HINT_NOADMIN));
    } else {
        UpdateTargetLabel(hwnd);
        if (g_hint) SetWindowTextW(g_hint, TR(TR_HINT_RESTART));
    }
}

static void OnCreate(HWND hwnd) {
    CreateUiFonts();
    BuildSetupPanel(hwnd);
    BuildMainPanel(hwnd);
    BuildFooter(hwnd);
    g_installed = IsInstalled();
    g_reinstall = FALSE;
    g_view = g_installed ? VIEW_MAIN : VIEW_SETUP;
    // Initial state: select the OTHER shell as the target, so a first
    // click on the main button actually changes something.
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind cur = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
    SetRadioForKind(hwnd, (cur == SHELL_WIN7EXPLORERRESTORER) ? SHELL_NATIVE : SHELL_WIN7EXPLORERRESTORER);
    CheckDlgButton(hwnd, IDC_CHK_LOGIN,
                   LogonAutoStartPresent() ? BST_CHECKED : BST_UNCHECKED);
    ApplyView(hwnd);
    RefreshStatus(hwnd);
    SetTimer(hwnd, TID_REFRESH, REFRESH_TIMER_MS, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == TID_INSTALL)
            PollInstaller(hwnd);
        else
            RefreshStatus(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctl = (HWND)lParam;
        int id = (int)GetWindowLongPtrW(ctl, GWLP_ID);
        if (IsLinkId(id)) {
            SetTextColor(hdc, RGB(0, 102, 204));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if (ctl == g_hint || ctl == g_natSub || ctl == g_status ||
            ctl == GetDlgItem(hwnd, IDC_ST_WIN7EXPLORERRESTORERPATH)) {
            SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_RADIO_NATIVE:
        case IDC_RADIO_WIN7EXPLORERRESTORER:
            if (HIWORD(wParam) == BN_CLICKED)
                UpdateTargetLabel(hwnd);
            return 0;

        case IDC_BTN_BROWSE:
            if (HIWORD(wParam) == BN_CLICKED)
                BrowseForWin7ExplorerRestorer(hwnd);
            return 0;

        case IDC_BTN_THEME:
            if (HIWORD(wParam) == BN_CLICKED)
                InstallTheme(hwnd);
            return 0;

        case IDC_CBO_SHLANG:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                // Shell-only (see ShellLangSave): persisted per-user under
                // our own key; applied via the child env at next switch.
                int sel = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
                ShellLangSave(sel);
            }
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

        case IDC_LINK_INFO:
            if (HIWORD(wParam) == STN_CLICKED)
                OpenLogonHelp(hwnd);
            return 0;

        case IDC_LINK_REINSTALL:
            if (HIWORD(wParam) == STN_CLICKED)
                EnterReinstall(hwnd);
            return 0;

        case IDC_LINK_UNINSTALL:
            if (HIWORD(wParam) == STN_CLICKED)
                DoUninstall(hwnd);
            return 0;

        case IDOK:
            if (g_view == VIEW_SETUP) {
                StartInstall(hwnd);
            } else if (g_view == VIEW_INSTALLING) {
                AbortInstall(hwnd, FALSE);
            } else {
                if (MessageBoxW(hwnd, TR(TR_WARN_CONFIRM),
                        L"7explorer Shell Switcher",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return 0;
                (void)DoSwitch(hwnd, SelectedTarget(hwnd), FALSE, FALSE);
                ApplyView(hwnd);
            }
            return 0;

        case IDCANCEL:
            if (g_view == VIEW_INSTALLING)
                AbortInstall(hwnd, FALSE);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        if (g_view == VIEW_INSTALLING)
            AbortInstall(hwnd, FALSE);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TID_REFRESH);
        KillTimer(hwnd, TID_INSTALL);
        AbortInstall(hwnd, TRUE);  // safety: never orphan the child
        g_installProc.Close();
        DestroyUiFonts();
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
            HWND w = FindWindowW(L"Win7ExplorerRestorerShellSwitcher", NULL);
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

    // UI language: WIN7EXPLORERRESTORER_LANG=it|en > primary system UI language (English
    // default; the tool always works in English).
    {
        WCHAR l[8];
        DWORD n = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_LANG", l, 7);
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
    //   --apply-win7explorerestorer        apply Win7ExplorerRestorer as shell (verified, retries for
    //                      ~60 s, restarts the hotkey resident)
    //   --logon            modifier for --apply-win7explorerestorer: running from the logon
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
        int mode = 0;   // 0=GUI, 1=win7explorerestorer, 2=native, 3=install, 4=uninstall,
                        // 5=hotkey, 6=recover
        BOOL logonLink = FALSE;
        for (int i = 1; i < argc; i++) {
            if (!lstrcmpiW(argv[i], L"--apply-win7explorerestorer"))        mode = 1;
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
            return ApplyWin7ExplorerRestorerRobust(logonLink);
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

    // GUI path only (headless modes above never reach this): opt out of
    // DPI virtualization so Dpx() scaling renders crisp, and load the
    // progress-bar class used by the Setup view.
    (void)SetProcessDPIAware();
    DpiInit();
    {
        INITCOMMONCONTROLSEX icc;
        ZeroMemory(&icc, sizeof(icc));
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Win7ExplorerRestorerShellSwitcher";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc))
        return 1;

    // Client area 480x330 @96dpi, DPI-scaled (unified setup UI).
    RECT want = { 0, 0, Dpx(480), Dpx(330) };
    AdjustWindowRectEx(&want,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, WS_EX_DLGMODALFRAME);
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"Win7ExplorerRestorerShellSwitcher",
        L"7explorer Shell Switcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        want.right - want.left, want.bottom - want.top,
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
