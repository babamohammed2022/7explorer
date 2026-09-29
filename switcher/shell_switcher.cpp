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

// ---- multilingual UI (test39): English default; the system UI language selects
// de/es/fr/it/ja/pl/pt-BR/ru/zh-CN when it matches, otherwise English.
// WIN7EXPLORERRESTORER_LANG/--lang=<code> forces one (codes: en it de es fr ja pl
// pt-BR ru zh-CN; legacy single letters "i"/"e" still mean it/en). The switcher
// itself must also work in English: every table is compile-count-checked and TR()
// falls back to English for any missing entry. Non-ASCII is written as \uXXXX so
// the source stays plain ASCII (no /utf-8 needed). The language choice affects
// ONLY this process (never any Windows-wide setting).
typedef enum { UI_EN = 0, UI_IT, UI_DE, UI_ES, UI_FR, UI_JA, UI_PL, UI_PTBR, UI_RU, UI_ZHCN, UI_LANG_COUNT } UiLang;
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
    // Shell-language combo entries, appended (test40).
    TR_CBO_DE, TR_CBO_ES, TR_CBO_FR, TR_CBO_JA,
    TR_CBO_PL, TR_CBO_PTBR, TR_CBO_RU, TR_CBO_ZHCN,
    TR_COUNT
} TRID;
static const WCHAR* TR_EN[] = {
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
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_IT[] = {
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
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_DE[] = {
    L"Wechselt die laufende Explorer-Shell zur Laufzeit. Keine Abmeldung n\u00f6tig.",
    L"Explorer-Shell ausw\u00e4hlen",
    L"Nativer Windows-Explorer",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(keine erkannt)",
    L"Unbekannter Explorer (siehe Pfad)",
    L"Wechseln",
    L"Abbrechen",
    L"Durchsuchen\u2026",
    L"Windows 7 Explorer automatisch bei der Anmeldung starten\r\n(setzt den "
    L"benutzerspezifischen Shell-Wert, reversibel \u2014 Details: "
    L"docs/avvio-al-login.md)",
    L"Aktuelle Shell: %s\r\nPID %lu \u2014 %s",
    L"Ziel: %s\r\n%s",
    L"Explorer wird neu gestartet.\r\nNicht gespeicherte Arbeit geht ggf. "
    L"verloren.\r\n\r\nFortfahren?",
    L"Zielprogramm nicht gefunden:\r\n%s\r\n\r\nEs wurde nichts gewechselt.",
    L"Stopp der aktuellen Shell verweigert: die ausf\u00fchrbare Datei\r\nist kein "
    L"erkannter Explorer:\r\n%s\r\n\r\nEs wurde nichts gewechselt.",
    L"Die gew\u00e4hlte Shell l\u00e4uft bereits.",
    L"Windows 7 Explorer konnte nicht gestartet werden:\r\n%s\r\nCreateProcess-Fehler "
    L"%lu.\r\n\r\nVersuche, die native Shell wiederherzustellen\u2026",
    L"KRITISCH: KEINE Shell konnte gestartet werden.\r\nAuch die native "
    L"Wiederherstellung schlug fehl (Fehler %lu).\r\n\r\nWiederherstellung: "
    L"Strg+Alt+Umschalt+S \u00f6ffnet diesen Switcher; oder Strg+Umschalt+Esc \u2192 "
    L"Task-Manager \u2192 Neuen Task ausf\u00fchren \u2192 %s",
    L"Die native Shell konnte nicht gestartet werden:\r\n%s\r\nCreateProcess-Fehler "
    L"%lu.\r\n\r\nStart von %s wiederholen?",
    L"%ls des automatischen Starts bei der Anmeldung war nicht m\u00f6glich (Fehler "
    L"%lu).\r\nSonst wurde nichts ge\u00e4ndert.",
    L"Windows 7 Explorer wird bei der Anmeldung gestartet. \u00dcbernommen (alles "
    L"reversibel):\r\n\u2022 benutzerspezifischer Shell-Wert (HKCU ...\\Winlogon\\Shell) \u2014 "
    L"vorheriger Wert gespeichert;\r\n\u2022 Verkn\u00fcpfung im Autostart-Ordner (Fallback + "
    L"Hotkey-Neustart);\r\n\u2022 Wiederherstellungsaufgabe \"7explorer Shell Recovery\" "
    L"(~30 s nach der Anmeldung:\r\n   l\u00e4uft die private Shell nicht, stellt sie "
    L"alles wieder her).\r\n\r\nZum R\u00fcckg\u00e4ngigmachen aller drei das Kontrollk\u00e4stchen "
    L"deaktivieren (der vorherige Wert wird\r\nbyte-identisch wiederhergestellt). "
    L"Details: docs/avvio-al-login.md",
    L"Die private Win7ExplorerRestorer-Datei w\u00e4hlen (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Alle Dateien\0*.*\0",
    L"Vorgang mit der Autostart-Verkn\u00fcpfung fehlgeschlagen (Fehler %lu).",
    L"Systemstandard",
    L"English",
    L"Italiano",
    L"Design anpassen\u2026",
    L"DEINE Windows-7-Designdatei w\u00e4hlen (aero.msstyles)",
    L"Designdateien\0*.msstyles\0Alle Dateien\0*.*\0",
    L"Design installiert nach:\r\n%s\r\n\r\nShell wechseln (z. B. nativ \u2192 "
    L"Win7ExplorerRestorer), um es anzuwenden.\r\nDie Datei geh\u00f6rt DIR: sie wurde "
    L"nur lokal kopiert, nichts heruntergeladen oder weitergegeben.",
    L"Die Designdatei konnte nicht kopiert werden (Fehler %lu):\r\n%s",
    L"Automatischer Start bei der Anmeldung nicht m\u00f6glich:\r\n%s\r\n\r\nIn diesem "
    L"Ordner m\u00fcssen explorer.exe UND wrp64.dll vorhanden sein\r\n(der Shell-Wert "
    L"zeigt nie auf fehlende Dateien).",
    L"Automatischer Start bei der Anmeldung entfernt.\r\nDer vorherige Shell-Wert "
    L"wurde wiederhergestellt, Fallback-Verkn\u00fcpfung\r\nund "
    L"Wiederherstellungsaufgabe wurden gel\u00f6scht.",
    L"Der benutzerspezifische Shell-Wert wurde gesetzt, aber die "
    L"Wiederherstellungsaufgabe\r\nkonnte nicht registriert werden (Fehler "
    L"%lu).\r\nDer automatische Start funktioniert trotzdem; es fehlt nur das "
    L"automatische\r\nSicherheitsnetz bei der Anmeldung (Details: "
    L"docs/avvio-al-login.md).",
    L"Windows 7 Explorer Restorer ist noch nicht installiert",
    L"Zur Verwendung muss eine Datei von Microsoft heruntergeladen, gepr\u00fcft und "
    L"vorbereitet werden. Eine Internetverbindung ist nur beim ersten Mal n\u00f6tig.",
    L"Windows 7 Explorer Restorer neu installieren?",
    L"Die private explorer.exe-Kopie wird erneut heruntergeladen und vorbereitet. "
    L"Falls sie verwendet wird, wird zuerst zum Windows-Explorer "
    L"zur\u00fcckgewechselt.",
    L"Installieren",
    L"Neu installieren",
    L"Abbrechen",
    L"Keine Administratorrechte n\u00f6tig.",
    L"Es dauert etwa eine Minute.",
    L"Installation l\u00e4uft\u2026",
    L"Installation fehlgeschlagen (Exitcode %lu).\r\n\r\n%s",
    L"Installation fehlgeschlagen \u2014 siehe Details.",
    L"Installation abgebrochen.",
    L"Das Installationsprogramm wurde nicht gefunden:\r\n%s\r\n\r\nWin7ExplorerRestorer."
    L"exe neben diesen Switcher kopieren (selber Ordner) und erneut versuchen.",
    L"Installationsprogramm nicht gefunden.",
    L"Das Installationsprogramm konnte nicht gestartet werden (Fehler %lu):\r\n%s",
    L"W\u00e4hlen, welcher Explorer als Windows-Shell dient. Der Wechsel ist sofort "
    L"wirksam, keine Abmeldung n\u00f6tig.",
    L"Standard-Systemshell",
    L"  \u2013 in Verwendung",
    L"Windows 7 Explorer Restorer bei jeder Anmeldung verwenden",
    L"Weitere Informationen",
    L"Neu installieren",
    L"Deinstallieren",
    L"Sprache",
    L"Win7ExplorerRestorer verwenden",
    L"Nativen Explorer verwenden",
    L"Der Desktop startet kurz neu.",
    L"Der automatische Start bei der Anmeldung setzt den benutzerspezifischen "
    L"Shell-Wert (HKCU) auf die private explorer.exe \u2014 der Windows-Standardweg, "
    L"ohne Rechteerh\u00f6hung, vollst\u00e4ndig reversibel (der vorherige Wert wird "
    L"gespeichert und byte-identisch wiederhergestellt).\r\n\r\nDazu kommen zwei "
    L"Sicherheitsnetze: eine Verkn\u00fcpfung im Autostart-Ordner und eine geplante "
    L"Aufgabe (\"7explorer Shell Recovery\"), die ~30 s nach der Anmeldung die "
    L"Shell pr\u00fcft.\r\n\r\nZum R\u00fcckg\u00e4ngigmachen das Kontrollk\u00e4stchen deaktivieren.",
    L"Neuinstallation abgeschlossen.\r\n\r\nJetzt zu Windows 7 Explorer Restorer "
    L"zur\u00fcckwechseln?",
    L"Windows 7 Explorer Restorer wird entfernt: es wird zum Windows-Explorer "
    L"zur\u00fcckgewechselt und die privaten Dateien werden gel\u00f6scht.\r\n\r\nFortfahren?",
    L"Windows 7 Explorer Restorer wurde entfernt.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_ES[] = {
    L"Cambia la shell de Explorer en ejecuci\u00f3n al momento. No hace falta cerrar "
    L"sesi\u00f3n.",
    L"Seleccionar la shell de Explorer",
    L"Explorador de Windows nativo",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(ninguna detectada)",
    L"Explorador desconocido (ver ruta)",
    L"Cambiar",
    L"Cancelar",
    L"Examinar\u2026",
    L"Iniciar Windows 7 Explorer autom\u00e1ticamente al iniciar sesi\u00f3n\r\n(establece el "
    L"valor Shell del usuario, reversible \u2014 detalles: docs/avvio-al-login.md)",
    L"Shell actual: %s\r\nPID %lu \u2014 %s",
    L"Destino: %s\r\n%s",
    L"Explorer se reiniciar\u00e1.\r\nEl trabajo no guardado podr\u00eda "
    L"perderse.\r\n\r\n\u00bfContinuar?",
    L"Ejecutable de destino no encontrado:\r\n%s\r\n\r\nNo se cambi\u00f3 nada.",
    L"Negada la detenci\u00f3n de la shell actual: su ejecutable\r\nno es un explorer "
    L"reconocido:\r\n%s\r\n\r\nNo se cambi\u00f3 nada.",
    L"La shell seleccionada ya est\u00e1 en ejecuci\u00f3n.",
    L"No se pudo iniciar Windows 7 Explorer:\r\n%s\r\nError %lu de "
    L"CreateProcess.\r\n\r\nIntentando restaurar la shell nativa\u2026",
    L"CR\u00cdTICO: no se pudo iniciar NINGUNA shell.\r\nLa restauraci\u00f3n nativa tambi\u00e9n "
    L"fall\u00f3 (error %lu).\r\n\r\nRecuperaci\u00f3n: Ctrl+Alt+May\u00fas+S abre este conmutador; "
    L"o Ctrl+May\u00fas+Esc \u2192 Administrador de tareas \u2192 Ejecutar nueva tarea \u2192 %s",
    L"No se pudo iniciar la shell nativa:\r\n%s\r\nError %lu de "
    L"CreateProcess.\r\n\r\n\u00bfReintentar el inicio de %s?",
    L"No se pudo %ls el inicio autom\u00e1tico al iniciar sesi\u00f3n (error %lu).\r\nNo se "
    L"cambi\u00f3 nada m\u00e1s.",
    L"Windows 7 Explorer se iniciar\u00e1 al iniciar sesi\u00f3n. Aplicado (todo "
    L"reversible):\r\n\u2022 valor Shell del usuario (HKCU ...\\Winlogon\\Shell) \u2014 valor "
    L"anterior guardado;\r\n\u2022 v\u00ednculo en la carpeta Inicio (reserva + reinicio con "
    L"tecla r\u00e1pida);\r\n\u2022 tarea de recuperaci\u00f3n \"7explorer Shell Recovery\" (~30 s "
    L"despu\u00e9s del inicio de sesi\u00f3n:\r\n   si la shell privada no est\u00e1 viva, lo "
    L"restaura todo).\r\n\r\nDesmarca la casilla para deshacer las tres cosas (el "
    L"valor anterior se restaura\r\nbyte por byte). Detalles: "
    L"docs/avvio-al-login.md",
    L"Selecciona el Win7ExplorerRestorer privado (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Todos los archivos\0*.*\0",
    L"Fall\u00f3 la operaci\u00f3n del v\u00ednculo en Inicio (error %lu).",
    L"Predeterminado del sistema",
    L"English",
    L"Italiano",
    L"Personalizar tema\u2026",
    L"Selecciona TU archivo de tema de Windows 7 (aero.msstyles)",
    L"Archivos de tema\0*.msstyles\0Todos los archivos\0*.*\0",
    L"Tema instalado en:\r\n%s\r\n\r\nCambia de shell (p. ej. nativa \u2192 "
    L"Win7ExplorerRestorer) para aplicarlo.\r\nEl archivo es TUYO: solo se copi\u00f3 "
    L"localmente, nada descargado ni compartido.",
    L"No se pudo copiar el archivo de tema (error %lu):\r\n%s",
    L"No se puede activar el inicio autom\u00e1tico al iniciar sesi\u00f3n:\r\n%s\r\n\r\nEn esa "
    L"carpeta deben existir explorer.exe y wrp64.dll\r\n(el valor Shell nunca "
    L"apunta a archivos ausentes).",
    L"Inicio autom\u00e1tico eliminado.\r\nSe restaur\u00f3 el valor Shell anterior y se "
    L"eliminaron el v\u00ednculo\r\nde reserva y la tarea de recuperaci\u00f3n.",
    L"El valor Shell del usuario se estableci\u00f3, pero la tarea de recuperaci\u00f3n\r\nno "
    L"se pudo registrar (error %lu).\r\nEl inicio autom\u00e1tico funciona igual; solo "
    L"falta la red de seguridad\r\nautom\u00e1tica al iniciar sesi\u00f3n (detalles: "
    L"docs/avvio-al-login.md).",
    L"Windows 7 Explorer Restorer a\u00fan no est\u00e1 instalado",
    L"Para usarlo hay que descargar un archivo de Microsoft, verificarlo y "
    L"prepararlo. Solo la primera vez se necesita conexi\u00f3n a Internet.",
    L"\u00bfReinstalar Windows 7 Explorer Restorer?",
    L"La copia privada de explorer.exe se descargar\u00e1 y preparar\u00e1 de nuevo. Si "
    L"est\u00e1 en uso, primero volver\u00e1s al Explorador de Windows.",
    L"Instalar",
    L"Reinstalar",
    L"Interrumpir",
    L"No se necesitan derechos de administrador.",
    L"Se necesita cerca de un minuto.",
    L"Instalaci\u00f3n en curso\u2026",
    L"Instalaci\u00f3n fallida (c\u00f3digo %lu).\r\n\r\n%s",
    L"Instalaci\u00f3n fallida \u2014 ver detalles.",
    L"Instalaci\u00f3n interrumpida.",
    L"No se encontr\u00f3 el instalador:\r\n%s\r\n\r\nCopia Win7ExplorerRestorer.exe junto a "
    L"este conmutador (misma carpeta) e int\u00e9ntalo de nuevo.",
    L"Instalador no encontrado.",
    L"No se pudo iniciar el instalador (error %lu):\r\n%s",
    L"Elige qu\u00e9 Explorer usar como shell de Windows. El cambio es inmediato y no "
    L"requiere cerrar sesi\u00f3n.",
    L"Shell predeterminada del sistema",
    L"  \u2013 en uso",
    L"Usar Windows 7 Explorer Restorer en cada inicio de sesi\u00f3n",
    L"M\u00e1s informaci\u00f3n",
    L"Reinstalar",
    L"Desinstalar",
    L"Idioma",
    L"Usar Win7ExplorerRestorer",
    L"Usar el Explorador nativo",
    L"El escritorio se reiniciar\u00e1 brevemente.",
    L"El inicio autom\u00e1tico establece el valor Shell del usuario (HKCU) en el "
    L"explorer.exe privado \u2014 el m\u00e9todo est\u00e1ndar de Windows, sin elevaci\u00f3n, "
    L"totalmente reversible (el valor anterior se guarda y se restaura byte por "
    L"byte).\r\n\r\nTambi\u00e9n a\u00f1ade dos redes de seguridad: un v\u00ednculo en la carpeta "
    L"Inicio y una tarea programada (\"7explorer Shell Recovery\") que comprueba la "
    L"shell ~30 s despu\u00e9s del inicio de sesi\u00f3n.\r\n\r\nDesmarca la casilla para "
    L"deshacerlo todo.",
    L"Reinstalaci\u00f3n completada.\r\n\r\n\u00bfVolver ahora a Windows 7 Explorer Restorer?",
    L"Windows 7 Explorer Restorer ser\u00e1 eliminado: volver\u00e1s al Explorador de "
    L"Windows y se borrar\u00e1n los archivos privados.\r\n\r\n\u00bfProceder?",
    L"Windows 7 Explorer Restorer fue eliminado.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_FR[] = {
    L"Bascule \u00e0 chaud la shell Explorer active. Aucune d\u00e9connexion requise.",
    L"S\u00e9lectionner la shell Explorer",
    L"Explorateur Windows natif",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(aucune d\u00e9tect\u00e9e)",
    L"Explorateur inconnu (voir le chemin)",
    L"Basculer",
    L"Annuler",
    L"Parcourir\u2026",
    L"D\u00e9marrer Windows 7 Explorer automatiquement \u00e0 l'ouverture de "
    L"session\r\n(d\u00e9finit la valeur Shell de l'utilisateur, r\u00e9versible \u2014 d\u00e9tails : "
    L"docs/avvio-al-login.md)",
    L"Shell actuelle : %s\r\nPID %lu \u2014 %s",
    L"Cible : %s\r\n%s",
    L"Explorer va red\u00e9marrer.\r\nLe travail non enregistr\u00e9 pourrait \u00eatre "
    L"perdu.\r\n\r\nContinuer ?",
    L"Ex\u00e9cutable cible introuvable :\r\n%s\r\n\r\nRien n'a \u00e9t\u00e9 bascul\u00e9.",
    L"Arr\u00eat de la shell actuelle refus\u00e9 : son ex\u00e9cutable\r\nn'est pas un explorer "
    L"reconnu :\r\n%s\r\n\r\nRien n'a \u00e9t\u00e9 bascul\u00e9.",
    L"La shell s\u00e9lectionn\u00e9e est d\u00e9j\u00e0 en cours d'ex\u00e9cution.",
    L"Impossible de d\u00e9marrer Windows 7 Explorer :\r\n%s\r\nErreur CreateProcess "
    L"%lu.\r\n\r\nTentative de restauration de la shell native\u2026",
    L"CRITIQUE : impossible de d\u00e9marrer AUCUNE shell.\r\nLa restauration native a "
    L"aussi \u00e9chou\u00e9 (erreur %lu).\r\n\r\nR\u00e9cup\u00e9ration : Ctrl+Alt+Maj+S ouvre ce "
    L"s\u00e9lecteur ; ou Ctrl+Maj+\u00c9chap \u2192 Gestionnaire des t\u00e2ches \u2192 Ex\u00e9cuter une "
    L"nouvelle t\u00e2che \u2192 %s",
    L"Impossible de d\u00e9marrer la shell native :\r\n%s\r\nErreur CreateProcess "
    L"%lu.\r\n\r\nR\u00e9essayer de d\u00e9marrer %s ?",
    L"Impossible de %ls le d\u00e9marrage automatique \u00e0 l'ouverture de session (erreur "
    L"%lu).\r\nRien d'autre n'a \u00e9t\u00e9 modifi\u00e9.",
    L"Windows 7 Explorer d\u00e9marrera \u00e0 l'ouverture de session. Appliqu\u00e9 (tout "
    L"r\u00e9versible) :\r\n\u2022 valeur Shell de l'utilisateur (HKCU ...\\Winlogon\\Shell) \u2014 "
    L"valeur pr\u00e9c\u00e9dente enregistr\u00e9e ;\r\n\u2022 lien dans le dossier D\u00e9marrage (secours "
    L"+ red\u00e9marrage par raccourci) ;\r\n\u2022 t\u00e2che de r\u00e9cup\u00e9ration \"7explorer Shell "
    L"Recovery\" (~30 s apr\u00e8s l'ouverture de session :\r\n   si la shell priv\u00e9e "
    L"n'est pas active, elle restaure tout).\r\n\r\nD\u00e9cochez la case pour tout "
    L"annuler (la valeur pr\u00e9c\u00e9dente est restaur\u00e9e\r\n\u00e0 l'octet pr\u00e8s). D\u00e9tails : "
    L"docs/avvio-al-login.md",
    L"S\u00e9lectionnez le Win7ExplorerRestorer priv\u00e9 (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Tous les fichiers\0*.*\0",
    L"\u00c9chec de l'op\u00e9ration sur le lien du dossier D\u00e9marrage (erreur %lu).",
    L"D\u00e9faut du syst\u00e8me",
    L"English",
    L"Italiano",
    L"Personnaliser le th\u00e8me\u2026",
    L"S\u00e9lectionnez VOTRE fichier de th\u00e8me Windows 7 (aero.msstyles)",
    L"Fichiers de th\u00e8me\0*.msstyles\0Tous les fichiers\0*.*\0",
    L"Th\u00e8me install\u00e9 dans :\r\n%s\r\n\r\nBasculez de shell (p. ex. native \u2192 "
    L"Win7ExplorerRestorer) pour l'appliquer.\r\nLe fichier reste \u00e0 VOUS : il a "
    L"seulement \u00e9t\u00e9 copi\u00e9 localement, rien de t\u00e9l\u00e9charg\u00e9 ni partag\u00e9.",
    L"Impossible de copier le fichier de th\u00e8me (erreur %lu) :\r\n%s",
    L"Impossible d'activer le d\u00e9marrage automatique \u00e0 l'ouverture de session "
    L":\r\n%s\r\n\r\nexplorer.exe et wrp64.dll doivent exister dans ce dossier\r\n(la "
    L"valeur Shell ne pointe jamais vers des fichiers manquants).",
    L"D\u00e9marrage automatique supprim\u00e9.\r\nLa valeur Shell pr\u00e9c\u00e9dente a \u00e9t\u00e9 restaur\u00e9e "
    L"; le lien\r\nde secours et la t\u00e2che de r\u00e9cup\u00e9ration ont \u00e9t\u00e9 supprim\u00e9s.",
    L"La valeur Shell de l'utilisateur a \u00e9t\u00e9 d\u00e9finie, mais la t\u00e2che de "
    L"r\u00e9cup\u00e9ration\r\nn'a pas pu \u00eatre inscrite (erreur %lu).\r\nLe d\u00e9marrage "
    L"automatique fonctionne quand m\u00eame ; il manque juste le filet\r\nde s\u00e9curit\u00e9 "
    L"automatique \u00e0 l'ouverture de session (d\u00e9tails : docs/avvio-al-login.md).",
    L"Windows 7 Explorer Restorer n'est pas encore install\u00e9",
    L"Pour l'utiliser, un fichier doit \u00eatre t\u00e9l\u00e9charg\u00e9 depuis Microsoft, v\u00e9rifi\u00e9 "
    L"et pr\u00e9par\u00e9. Une connexion Internet n'est n\u00e9cessaire que la premi\u00e8re fois.",
    L"R\u00e9installer Windows 7 Explorer Restorer ?",
    L"La copie priv\u00e9e d'explorer.exe sera de nouveau t\u00e9l\u00e9charg\u00e9e et pr\u00e9par\u00e9e. Si "
    L"elle est utilis\u00e9e, vous reviendrez d'abord \u00e0 l'Explorateur Windows.",
    L"Installer",
    L"R\u00e9installer",
    L"Interrompre",
    L"Aucun droit d'administrateur requis.",
    L"Environ une minute est n\u00e9cessaire.",
    L"Installation en cours\u2026",
    L"\u00c9chec de l'installation (code %lu).\r\n\r\n%s",
    L"\u00c9chec de l'installation \u2014 voir les d\u00e9tails.",
    L"Installation interrompue.",
    L"Programme d'installation introuvable :\r\n%s\r\n\r\nCopiez "
    L"Win7ExplorerRestorer.exe \u00e0 c\u00f4t\u00e9 de ce s\u00e9lecteur (m\u00eame dossier) et "
    L"r\u00e9essayez.",
    L"Programme d'installation introuvable.",
    L"Impossible de d\u00e9marrer le programme d'installation (erreur %lu) :\r\n%s",
    L"Choisissez l'Explorateur \u00e0 utiliser comme shell Windows. Le basculement est "
    L"imm\u00e9diat, sans d\u00e9connexion.",
    L"Shell par d\u00e9faut du syst\u00e8me",
    L"  \u2013 utilis\u00e9",
    L"Utiliser Windows 7 Explorer Restorer \u00e0 chaque ouverture de session",
    L"Plus d'informations",
    L"R\u00e9installer",
    L"D\u00e9sinstaller",
    L"Langue",
    L"Utiliser Win7ExplorerRestorer",
    L"Utiliser l'Explorateur natif",
    L"Le bureau red\u00e9marrera bri\u00e8vement.",
    L"Le d\u00e9marrage automatique d\u00e9finit la valeur Shell de l'utilisateur (HKCU) "
    L"sur l'explorer.exe priv\u00e9 \u2014 la m\u00e9thode standard de Windows, sans \u00e9l\u00e9vation, "
    L"enti\u00e8rement r\u00e9versible (la valeur pr\u00e9c\u00e9dente est enregistr\u00e9e et restaur\u00e9e \u00e0 "
    L"l'octet pr\u00e8s).\r\n\r\nIl ajoute aussi deux filets de s\u00e9curit\u00e9 : un lien dans le "
    L"dossier D\u00e9marrage et une t\u00e2che planifi\u00e9e (\"7explorer Shell Recovery\") qui "
    L"v\u00e9rifie la shell ~30 s apr\u00e8s l'ouverture de session.\r\n\r\nD\u00e9cochez la case "
    L"pour tout annuler.",
    L"R\u00e9installation termin\u00e9e.\r\n\r\nRevenir maintenant \u00e0 Windows 7 Explorer "
    L"Restorer ?",
    L"Windows 7 Explorer Restorer va \u00eatre supprim\u00e9 : vous reviendrez \u00e0 "
    L"l'Explorateur Windows et les fichiers priv\u00e9s seront effac\u00e9s.\r\n\r\nContinuer ?",
    L"Windows 7 Explorer Restorer a \u00e9t\u00e9 supprim\u00e9.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_JA[] = {
    L"\u5b9f\u884c\u4e2d\u306e Explorer \u30b7\u30a7\u30eb\u3092\u5373\u6642\u306b\u5207\u308a\u66ff\u3048\u307e\u3059\u3002\u30ed\u30b0\u30aa\u30d5\u306f\u4e0d\u8981\u3067\u3059\u3002",
    L"Explorer \u30b7\u30a7\u30eb\u3092\u9078\u629e",
    L"\u30cd\u30a4\u30c6\u30a3\u30d6 Windows \u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(\u691c\u51fa\u306a\u3057)",
    L"\u4e0d\u660e\u306a\u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc (\u30d1\u30b9\u3092\u53c2\u7167)",
    L"\u5207\u308a\u66ff\u3048",
    L"\u30ad\u30e3\u30f3\u30bb\u30eb",
    L"\u53c2\u7167\u2026",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306b Windows 7 Explorer \u3092\u81ea\u52d5\u7684\u306b\u958b\u59cb\u3059\u308b\r\n(\u30e6\u30fc\u30b6\u30fc\u3054\u3068\u306e Shell \u5024\u3092\u8a2d\u5b9a\u3001\u5143\u306b\u623b\u305b\u307e\u3059 \u2014 \u8a73\u7d30: "
    L"docs/avvio-al-login.md)",
    L"\u73fe\u5728\u306e\u30b7\u30a7\u30eb: %s\r\nPID %lu \u2014 %s",
    L"\u5bfe\u8c61: %s\r\n%s",
    L"Explorer \u304c\u518d\u8d77\u52d5\u3055\u308c\u307e\u3059\u3002\r\n\u4fdd\u5b58\u3057\u3066\u3044\u306a\u3044\u4f5c\u696d\u306f\u5931\u308f\u308c\u308b\u53ef\u80fd\u6027\u304c\u3042\u308a\u307e\u3059\u3002\r\n\r\n\u7d9a\u884c\u3057\u307e\u3059\u304b?",
    L"\u5bfe\u8c61\u306e\u5b9f\u884c\u30d5\u30a1\u30a4\u30eb\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093:\r\n%s\r\n\r\n\u4f55\u3082\u5207\u308a\u66ff\u3048\u307e\u305b\u3093\u3067\u3057\u305f\u3002",
    L"\u73fe\u5728\u306e\u30b7\u30a7\u30eb\u3092\u505c\u6b62\u3067\u304d\u307e\u305b\u3093: \u5b9f\u884c\u30d5\u30a1\u30a4\u30eb\u304c\r\n\u8a8d\u8b58\u3055\u308c\u305f\u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc\u3067\u306f\u3042\u308a\u307e\u305b\u3093:\r\n%s\r\n\r\n\u4f55\u3082\u5207\u308a\u66ff\u3048\u307e\u305b\u3093\u3067\u3057\u305f\u3002",
    L"\u9078\u629e\u3057\u305f\u30b7\u30a7\u30eb\u306f\u65e2\u306b\u5b9f\u884c\u4e2d\u3067\u3059\u3002",
    L"Windows 7 Explorer \u3092\u958b\u59cb\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f:\r\n%s\r\nCreateProcess \u30a8\u30e9\u30fc %lu\u3002\r\n\r\n\u30cd\u30a4\u30c6\u30a3\u30d6 "
    L"\u30b7\u30a7\u30eb\u306e\u5fa9\u5143\u3092\u8a66\u307f\u3066\u3044\u307e\u3059\u2026",
    L"\u91cd\u5927: \u3069\u306e\u30b7\u30a7\u30eb\u3082\u958b\u59cb\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f\u3002\r\n\u30cd\u30a4\u30c6\u30a3\u30d6\u306e\u5fa9\u5143\u3082\u5931\u6557\u3057\u307e\u3057\u305f (\u30a8\u30e9\u30fc %lu)\u3002\r\n\r\n\u5fa9\u65e7: Ctrl+Alt+Shift+S "
    L"\u3067\u3053\u306e\u30b9\u30a4\u30c3\u30c1\u30e3\u30fc\u3092\u958b\u304d\u307e\u3059\u3002\u307e\u305f\u306f Ctrl+Shift+Esc \u2192 \u30bf\u30b9\u30af \u30de\u30cd\u30fc\u30b8\u30e3\u30fc \u2192 \u65b0\u3057\u3044\u30bf\u30b9\u30af\u306e\u5b9f\u884c \u2192 %s",
    L"\u30cd\u30a4\u30c6\u30a3\u30d6 \u30b7\u30a7\u30eb\u3092\u958b\u59cb\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f:\r\n%s\r\nCreateProcess \u30a8\u30e9\u30fc %lu\u3002\r\n\r\n%s \u306e\u958b\u59cb\u3092\u518d\u8a66\u884c\u3057\u307e\u3059\u304b?",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306e\u81ea\u52d5\u958b\u59cb\u3092%ls\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f (\u30a8\u30e9\u30fc %lu)\u3002\r\n\u4ed6\u306b\u306f\u4f55\u3082\u5909\u66f4\u3057\u3066\u3044\u307e\u305b\u3093\u3002",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306b Windows 7 Explorer \u304c\u958b\u59cb\u3055\u308c\u307e\u3059\u3002\u9069\u7528\u6e08\u307f (\u3059\u3079\u3066\u5143\u306b\u623b\u305b\u307e\u3059):\r\n\u2022 \u30e6\u30fc\u30b6\u30fc\u3054\u3068\u306e Shell \u5024 "
    L"(HKCU ...\\Winlogon\\Shell) \u2014 \u4ee5\u524d\u306e\u5024\u3092\u4fdd\u5b58\u6e08\u307f;\r\n\u2022 \u30b9\u30bf\u30fc\u30c8\u30a2\u30c3\u30d7 \u30d5\u30a9\u30eb\u30c0\u30fc\u5185\u306e\u30ea\u30f3\u30af (\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af + "
    L"\u30db\u30c3\u30c8\u30ad\u30fc\u518d\u8d77\u52d5);\r\n\u2022 \u56de\u5fa9\u30bf\u30b9\u30af \"7explorer Shell Recovery\" (\u30ed\u30b0\u30aa\u30f3\u7d04 30 \u79d2\u5f8c:\r\n   \u30d7\u30e9\u30a4\u30d9\u30fc\u30c8 "
    L"\u30b7\u30a7\u30eb\u304c\u52d5\u4f5c\u3057\u3066\u3044\u306a\u3051\u308c\u3070\u3059\u3079\u3066\u5fa9\u5143\u3057\u307e\u3059)\u3002\r\n\r\n3 \u3064\u3059\u3079\u3066\u3092\u5143\u306b\u623b\u3059\u306b\u306f\u30c1\u30a7\u30c3\u30af\u3092\u5916\u3057\u3066\u304f\u3060\u3055\u3044 "
    L"(\u4ee5\u524d\u306e\u5024\u304c\r\n\u30d0\u30a4\u30c8\u5358\u4f4d\u3067\u5fa9\u5143\u3055\u308c\u307e\u3059)\u3002\u8a73\u7d30: docs/avvio-al-login.md",
    L"\u30d7\u30e9\u30a4\u30d9\u30fc\u30c8\u306e Win7ExplorerRestorer \u3092\u9078\u629e (explorer.exe)",
    L"explorer.exe\0explorer.exe\0\u3059\u3079\u3066\u306e\u30d5\u30a1\u30a4\u30eb\0*.*\0",
    L"\u30b9\u30bf\u30fc\u30c8\u30a2\u30c3\u30d7 \u30d5\u30a9\u30eb\u30c0\u30fc\u306e\u30ea\u30f3\u30af\u64cd\u4f5c\u306b\u5931\u6557\u3057\u307e\u3057\u305f (\u30a8\u30e9\u30fc %lu)\u3002",
    L"\u30b7\u30b9\u30c6\u30e0\u306e\u65e2\u5b9a\u5024",
    L"English",
    L"Italiano",
    L"\u30c6\u30fc\u30de\u306e\u30ab\u30b9\u30bf\u30de\u30a4\u30ba\u2026",
    L"\u304a\u4f7f\u3044\u306e Windows 7 \u30c6\u30fc\u30de \u30d5\u30a1\u30a4\u30eb\u3092\u9078\u629e (aero.msstyles)",
    L"\u30c6\u30fc\u30de \u30d5\u30a1\u30a4\u30eb\0*.msstyles\0\u3059\u3079\u3066\u306e\u30d5\u30a1\u30a4\u30eb\0*.*\0",
    L"\u30c6\u30fc\u30de\u306e\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u5148:\r\n%s\r\n\r\n\u9069\u7528\u3059\u308b\u306b\u306f\u30b7\u30a7\u30eb\u3092\u5207\u308a\u66ff\u3048\u3066\u304f\u3060\u3055\u3044 (\u4f8b: \u30cd\u30a4\u30c6\u30a3\u30d6 \u2192 "
    L"Win7ExplorerRestorer)\u3002\r\n\u30d5\u30a1\u30a4\u30eb\u306f\u304a\u5ba2\u69d8\u306e\u3082\u306e\u3067\u3059: \u30ed\u30fc\u30ab\u30eb\u306b\u30b3\u30d4\u30fc\u3057\u305f\u3060\u3051\u3067\u3001\u30c0\u30a6\u30f3\u30ed\u30fc\u30c9\u3084\u5171\u6709\u306f\u3057\u3066\u3044\u307e\u305b\u3093\u3002",
    L"\u30c6\u30fc\u30de \u30d5\u30a1\u30a4\u30eb\u3092\u30b3\u30d4\u30fc\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f (\u30a8\u30e9\u30fc %lu):\r\n%s",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306e\u81ea\u52d5\u958b\u59cb\u3092\u6709\u52b9\u306b\u3067\u304d\u307e\u305b\u3093:\r\n%s\r\n\r\n\u305d\u306e\u30d5\u30a9\u30eb\u30c0\u30fc\u306b\u306f explorer.exe \u3068 wrp64.dll "
    L"\u306e\u4e21\u65b9\u304c\u5fc5\u8981\u3067\u3059\r\n(Shell \u5024\u304c\u5b58\u5728\u3057\u306a\u3044\u30d5\u30a1\u30a4\u30eb\u3092\u6307\u3059\u3053\u3068\u306f\u3042\u308a\u307e\u305b\u3093)\u3002",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306e\u81ea\u52d5\u958b\u59cb\u3092\u524a\u9664\u3057\u307e\u3057\u305f\u3002\r\n\u4ee5\u524d\u306e Shell \u5024\u3092\u5fa9\u5143\u3057\u3001\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af \u30ea\u30f3\u30af\u3068\r\n\u56de\u5fa9\u30bf\u30b9\u30af\u3092\u524a\u9664\u3057\u307e\u3057\u305f\u3002",
    L"\u30e6\u30fc\u30b6\u30fc\u3054\u3068\u306e Shell \u5024\u306f\u8a2d\u5b9a\u3055\u308c\u307e\u3057\u305f\u304c\u3001\u56de\u5fa9\u30bf\u30b9\u30af\u3092\r\n\u767b\u9332\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f (\u30a8\u30e9\u30fc "
    L"%lu)\u3002\r\n\u81ea\u52d5\u958b\u59cb\u306f\u52d5\u4f5c\u3057\u307e\u3059\u3002\u30ed\u30b0\u30aa\u30f3\u6642\u306e\u81ea\u52d5\u5b89\u5168\u30cd\u30c3\u30c8\u304c\r\n\u306a\u3044\u3060\u3051\u3067\u3059 (\u8a73\u7d30: docs/avvio-al-login.md)\u3002",
    L"Windows 7 Explorer Restorer \u306f\u307e\u3060\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3055\u308c\u3066\u3044\u307e\u305b\u3093",
    L"\u4f7f\u7528\u3059\u308b\u306b\u306f\u3001Microsoft \u304b\u3089\u30d5\u30a1\u30a4\u30eb\u3092\u30c0\u30a6\u30f3\u30ed\u30fc\u30c9\u3057\u3066\u691c\u8a3c\u30fb\u6e96\u5099\u3059\u308b\u5fc5\u8981\u304c\u3042\u308a\u307e\u3059\u3002\u30a4\u30f3\u30bf\u30fc\u30cd\u30c3\u30c8\u63a5\u7d9a\u304c\u5fc5\u8981\u306a\u306e\u306f\u521d\u56de\u306e\u307f\u3067\u3059\u3002",
    L"Windows 7 Explorer Restorer \u3092\u518d\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3057\u307e\u3059\u304b?",
    L"\u30d7\u30e9\u30a4\u30d9\u30fc\u30c8\u306e explorer.exe \u30b3\u30d4\u30fc\u3092\u518d\u5ea6\u30c0\u30a6\u30f3\u30ed\u30fc\u30c9\u3057\u3066\u6e96\u5099\u3057\u307e\u3059\u3002\u4f7f\u7528\u4e2d\u306e\u5834\u5408\u306f\u3001\u5148\u306b\u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc\u306b\u623b\u308a\u307e\u3059\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb",
    L"\u518d\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb",
    L"\u4e2d\u6b62",
    L"\u7ba1\u7406\u8005\u6a29\u9650\u306f\u4e0d\u8981\u3067\u3059\u3002",
    L"\u7d04 1 \u5206\u304b\u304b\u308a\u307e\u3059\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u5b9f\u884c\u4e2d\u2026",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u306b\u5931\u6557\u3057\u307e\u3057\u305f (\u7d42\u4e86\u30b3\u30fc\u30c9 %lu)\u3002\r\n\r\n%s",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u306b\u5931\u6557\u3057\u307e\u3057\u305f \u2014 \u8a73\u7d30\u3092\u53c2\u7167\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3092\u4e2d\u6b62\u3057\u307e\u3057\u305f\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093:\r\n%s\r\n\r\nWin7ExplorerRestorer.exe \u3092\u3053\u306e\u30b9\u30a4\u30c3\u30c1\u30e3\u30fc\u306e\u96a3 (\u540c\u3058\u30d5\u30a9\u30eb\u30c0\u30fc) "
    L"\u306b\u30b3\u30d4\u30fc\u3057\u3066\u518d\u8a66\u884c\u3057\u3066\u304f\u3060\u3055\u3044\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093\u3002",
    L"\u30a4\u30f3\u30b9\u30c8\u30fc\u30e9\u30fc\u3092\u958b\u59cb\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f (\u30a8\u30e9\u30fc %lu):\r\n%s",
    L"Windows \u30b7\u30a7\u30eb\u3068\u3057\u3066\u4f7f\u7528\u3059\u308b Explorer \u3092\u9078\u629e\u3057\u3066\u304f\u3060\u3055\u3044\u3002\u5207\u308a\u66ff\u3048\u306f\u5373\u6642\u3067\u3001\u30ed\u30b0\u30aa\u30d5\u306f\u4e0d\u8981\u3067\u3059\u3002",
    L"\u65e2\u5b9a\u306e\u30b7\u30b9\u30c6\u30e0 \u30b7\u30a7\u30eb",
    L"  \u2013 \u4f7f\u7528\u4e2d",
    L"\u30ed\u30b0\u30aa\u30f3\u3054\u3068\u306b Windows 7 Explorer Restorer \u3092\u4f7f\u7528\u3059\u308b",
    L"\u8a73\u7d30\u60c5\u5831",
    L"\u518d\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb",
    L"\u30a2\u30f3\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb",
    L"\u8a00\u8a9e",
    L"Win7ExplorerRestorer \u3092\u4f7f\u7528",
    L"\u30cd\u30a4\u30c6\u30a3\u30d6 \u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc\u3092\u4f7f\u7528",
    L"\u30c7\u30b9\u30af\u30c8\u30c3\u30d7\u304c\u77ed\u6642\u9593\u518d\u8d77\u52d5\u3057\u307e\u3059\u3002",
    L"\u30ed\u30b0\u30aa\u30f3\u6642\u306e\u81ea\u52d5\u958b\u59cb\u3067\u306f\u3001\u30e6\u30fc\u30b6\u30fc\u3054\u3068\u306e Shell \u5024 (HKCU) \u3092\u30d7\u30e9\u30a4\u30d9\u30fc\u30c8\u306e explorer.exe \u306b\u8a2d\u5b9a\u3057\u307e\u3059 \u2014 \u6a19\u6e96\u7684\u306a "
    L"Windows \u306e\u65b9\u6cd5\u3067\u3001\u6607\u683c\u4e0d\u8981\u3001\u5b8c\u5168\u306b\u5143\u306b\u623b\u305b\u307e\u3059 (\u4ee5\u524d\u306e\u5024\u306f\u4fdd\u5b58\u3055\u308c\u30d0\u30a4\u30c8\u5358\u4f4d\u3067\u5fa9\u5143\u3055\u308c\u307e\u3059)\u3002\r\n\r\n\u3055\u3089\u306b 2 "
    L"\u3064\u306e\u5b89\u5168\u30cd\u30c3\u30c8\u3092\u8ffd\u52a0\u3057\u307e\u3059: \u30b9\u30bf\u30fc\u30c8\u30a2\u30c3\u30d7 \u30d5\u30a9\u30eb\u30c0\u30fc\u5185\u306e\u30ea\u30f3\u30af\u3068\u3001\u30ed\u30b0\u30aa\u30f3\u7d04 30 \u79d2\u5f8c\u306b\u30b7\u30a7\u30eb\u3092\u78ba\u8a8d\u3059\u308b\u30b9\u30b1\u30b8\u30e5\u30fc\u30eb \u30bf\u30b9\u30af "
    L"(\"7explorer Shell Recovery\") \u3067\u3059\u3002\r\n\r\n\u3059\u3079\u3066\u3092\u5143\u306b\u623b\u3059\u306b\u306f\u30c1\u30a7\u30c3\u30af\u3092\u5916\u3057\u3066\u304f\u3060\u3055\u3044\u3002",
    L"\u518d\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u304c\u5b8c\u4e86\u3057\u307e\u3057\u305f\u3002\r\n\r\n\u4eca\u3059\u3050 Windows 7 Explorer Restorer \u306b\u623b\u308a\u307e\u3059\u304b?",
    L"Windows 7 Explorer Restorer \u3092\u524a\u9664\u3057\u307e\u3059: \u30a8\u30af\u30b9\u30d7\u30ed\u30fc\u30e9\u30fc\u306b\u623b\u308a\u3001\u30d7\u30e9\u30a4\u30d9\u30fc\u30c8 "
    L"\u30d5\u30a1\u30a4\u30eb\u304c\u524a\u9664\u3055\u308c\u307e\u3059\u3002\r\n\r\n\u7d9a\u884c\u3057\u307e\u3059\u304b?",
    L"Windows 7 Explorer Restorer \u3092\u524a\u9664\u3057\u307e\u3057\u305f\u3002",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_PL[] = {
    L"Prze\u0142\u0105cza dzia\u0142aj\u0105c\u0105 pow\u0142ok\u0119 Eksploratora w locie. Wylogowanie nie jest "
    L"potrzebne.",
    L"Wybierz pow\u0142ok\u0119 Eksploratora",
    L"Natywny Eksplorator Windows",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(nie wykryto \u017cadnej)",
    L"Nieznany eksplorator (zobacz \u015bcie\u017ck\u0119)",
    L"Prze\u0142\u0105cz",
    L"Anuluj",
    L"Przegl\u0105daj\u2026",
    L"Uruchom Eksploratora Windows 7 automatycznie przy logowaniu\r\n(ustawia "
    L"warto\u015b\u0107 Shell u\u017cytkownika, odwracalne \u2014 szczeg\u00f3\u0142y: docs/avvio-al-login.md)",
    L"Bie\u017c\u0105ca pow\u0142oka: %s\r\nPID %lu \u2014 %s",
    L"Cel: %s\r\n%s",
    L"Eksplorator zostanie uruchomiony ponownie.\r\nNiezapisana praca mo\u017ce zosta\u0107 "
    L"utracona.\r\n\r\nKontynuowa\u0107?",
    L"Nie znaleziono docelowego pliku wykonywalnego:\r\n%s\r\n\r\nNiczego nie "
    L"prze\u0142\u0105czono.",
    L"Odmowa zatrzymania bie\u017c\u0105cej pow\u0142oki: jej plik wykonywalny\r\nnie jest "
    L"rozpoznanym eksploratorem:\r\n%s\r\n\r\nNiczego nie prze\u0142\u0105czono.",
    L"Wybrana pow\u0142oka ju\u017c dzia\u0142a.",
    L"Nie mo\u017cna uruchomi\u0107 Eksploratora Windows 7:\r\n%s\r\nB\u0142\u0105d CreateProcess "
    L"%lu.\r\n\r\nPr\u00f3ba przywr\u00f3cenia pow\u0142oki natywnej\u2026",
    L"KRYTYCZNE: nie mo\u017cna uruchomi\u0107 \u017bADNEJ pow\u0142oki.\r\nPrzywracanie natywne te\u017c "
    L"si\u0119 nie powiod\u0142o (b\u0142\u0105d %lu).\r\n\r\nOdzyskiwanie: Ctrl+Alt+Shift+S otwiera ten "
    L"prze\u0142\u0105cznik; lub Ctrl+Shift+Esc \u2192 Mened\u017cer zada\u0144 \u2192 Uruchom nowe zadanie \u2192 "
    L"%s",
    L"Nie mo\u017cna uruchomi\u0107 pow\u0142oki natywnej:\r\n%s\r\nB\u0142\u0105d CreateProcess "
    L"%lu.\r\n\r\nPonowi\u0107 uruchamianie %s?",
    L"Nie mo\u017cna %ls automatycznego uruchamiania przy logowaniu (b\u0142\u0105d "
    L"%lu).\r\nNiczego innego nie zmieniono.",
    L"Eksplorator Windows 7 b\u0119dzie uruchamiany przy logowaniu. Zastosowano "
    L"(wszystko odwracalne):\r\n\u2022 warto\u015b\u0107 Shell u\u017cytkownika (HKCU "
    L"...\\Winlogon\\Shell) \u2014 poprzednia warto\u015b\u0107 zapisana;\r\n\u2022 \u0142\u0105cze w folderze "
    L"Autostart (awaryjne + restart skr\u00f3tem);\r\n\u2022 zadanie odzyskiwania \"7explorer "
    L"Shell Recovery\" (~30 s po zalogowaniu:\r\n   je\u015bli prywatna pow\u0142oka nie "
    L"dzia\u0142a, przywr\u00f3ca wszystko).\r\n\r\nOdznacz pole, aby cofn\u0105\u0107 wszystkie trzy "
    L"(poprzednia warto\u015b\u0107 zostanie\r\nprzywr\u00f3cona bajt w bajt). Szczeg\u00f3\u0142y: "
    L"docs/avvio-al-login.md",
    L"Wybierz prywatny Win7ExplorerRestorer (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Wszystkie pliki\0*.*\0",
    L"Operacja \u0142\u0105cza w folderze Autostart nie powiod\u0142a si\u0119 (b\u0142\u0105d %lu).",
    L"Domy\u015blne systemu",
    L"English",
    L"Italiano",
    L"Dostosuj motyw\u2026",
    L"Wybierz TW\u00d3J plik motywu Windows 7 (aero.msstyles)",
    L"Pliki motyw\u00f3w\0*.msstyles\0Wszystkie pliki\0*.*\0",
    L"Motyw zainstalowany w:\r\n%s\r\n\r\nPrze\u0142\u0105cz pow\u0142ok\u0119 (np. natywna \u2192 "
    L"Win7ExplorerRestorer), aby go zastosowa\u0107.\r\nPlik jest TW\u00d3J: zosta\u0142 tylko "
    L"skopiowany lokalnie, nic nie pobrano ani nie udost\u0119pniono.",
    L"Nie mo\u017cna skopiowa\u0107 pliku motywu (b\u0142\u0105d %lu):\r\n%s",
    L"Nie mo\u017cna w\u0142\u0105czy\u0107 automatycznego uruchamiania przy logowaniu:\r\n%s\r\n\r\nW tym "
    L"folderze musz\u0105 istnie\u0107 explorer.exe i wrp64.dll\r\n(warto\u015b\u0107 Shell nigdy nie "
    L"wskazuje brakuj\u0105cych plik\u00f3w).",
    L"Automatyczne uruchamianie przy logowaniu usuni\u0119te.\r\nPoprzednia warto\u015b\u0107 "
    L"Shell zosta\u0142a przywr\u00f3cona, a \u0142\u0105cze\r\nawaryjne i zadanie odzyskiwania "
    L"usuni\u0119te.",
    L"Warto\u015b\u0107 Shell u\u017cytkownika zosta\u0142a ustawiona, ale zadania odzyskiwania\r\nnie "
    L"mo\u017cna by\u0142o zarejestrowa\u0107 (b\u0142\u0105d %lu).\r\nAutomatyczne uruchamianie dzia\u0142a; "
    L"brakuje tylko automatycznej\r\nsiatki bezpiecze\u0144stwa przy logowaniu "
    L"(szczeg\u00f3\u0142y: docs/avvio-al-login.md).",
    L"Program Windows 7 Explorer Restorer nie jest jeszcze zainstalowany",
    L"Aby go u\u017cywa\u0107, trzeba pobra\u0107 plik od Microsoft, zweryfikowa\u0107 go i "
    L"przygotowa\u0107. Po\u0142\u0105czenie z Internetem jest potrzebne tylko za pierwszym "
    L"razem.",
    L"Zainstalowa\u0107 ponownie program Windows 7 Explorer Restorer?",
    L"Prywatna kopia explorer.exe zostanie pobrana i przygotowana ponownie. Je\u015bli "
    L"jest u\u017cywana, najpierw nast\u0105pi powr\u00f3t do Eksploratora Windows.",
    L"Zainstaluj",
    L"Zainstaluj ponownie",
    L"Przerwij",
    L"Uprawnienia administratora nie s\u0105 potrzebne.",
    L"Potrzeba oko\u0142o minuty.",
    L"Trwa instalacja\u2026",
    L"Instalacja nie powiod\u0142a si\u0119 (kod wyj\u015bcia %lu).\r\n\r\n%s",
    L"Instalacja nie powiod\u0142a si\u0119 \u2014 zobacz szczeg\u00f3\u0142y.",
    L"Instalacja przerwana.",
    L"Nie znaleziono instalatora:\r\n%s\r\n\r\nSkopiuj Win7ExplorerRestorer.exe obok "
    L"tego prze\u0142\u0105cznika (ten sam folder) i spr\u00f3buj ponownie.",
    L"Nie znaleziono instalatora.",
    L"Nie mo\u017cna uruchomi\u0107 instalatora (b\u0142\u0105d %lu):\r\n%s",
    L"Wybierz Eksploratora u\u017cywanego jako pow\u0142oka Windows. Prze\u0142\u0105czenie jest "
    L"natychmiastowe i nie wymaga wylogowania.",
    L"Domy\u015blna pow\u0142oka systemu",
    L"  \u2013 w u\u017cyciu",
    L"U\u017cywaj Windows 7 Explorer Restorer przy ka\u017cdym logowaniu",
    L"Wi\u0119cej informacji",
    L"Zainstaluj ponownie",
    L"Odinstaluj",
    L"J\u0119zyk",
    L"U\u017cywaj Win7ExplorerRestorer",
    L"U\u017cywaj natywnego Eksploratora",
    L"Pulpit zostanie na chwil\u0119 uruchomiony ponownie.",
    L"Automatyczne uruchamianie przy logowaniu ustawia warto\u015b\u0107 Shell u\u017cytkownika "
    L"(HKCU) na prywatny explorer.exe \u2014 standardowa metoda Windows, bez "
    L"podnoszenia uprawnie\u0144, w pe\u0142ni odwracalna (poprzednia warto\u015b\u0107 jest "
    L"zapisywana i przywr\u00f3cana bajt w bajt).\r\n\r\nDodaje te\u017c dwie siatki "
    L"bezpiecze\u0144stwa: \u0142\u0105cze w folderze Autostart i zaplanowane zadanie "
    L"(\"7explorer Shell Recovery\"), kt\u00f3re sprawdza pow\u0142ok\u0119 ~30 s po "
    L"zalogowaniu.\r\n\r\nOdznacz pole, aby cofn\u0105\u0107 wszystko.",
    L"Ponowna instalacja zako\u0144czona.\r\n\r\nWr\u00f3ci\u0107 teraz do Windows 7 Explorer "
    L"Restorer?",
    L"Program Windows 7 Explorer Restorer zostanie usuni\u0119ty: nast\u0105pi powr\u00f3t do "
    L"Eksploratora Windows, a prywatne pliki zostan\u0105 usuni\u0119te.\r\n\r\nKontynuowa\u0107?",
    L"Program Windows 7 Explorer Restorer zosta\u0142 usuni\u0119ty.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_PTBR[] = {
    L"Alterna a shell do Explorer em execu\u00e7\u00e3o imediatamente. N\u00e3o \u00e9 preciso sair.",
    L"Selecionar a shell do Explorer",
    L"Explorador de Arquivos nativo",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(nenhuma detectada)",
    L"Explorer desconhecido (ver caminho)",
    L"Alternar",
    L"Cancelar",
    L"Procurar\u2026",
    L"Iniciar o Windows 7 Explorer automaticamente ao entrar\r\n(define o valor "
    L"Shell do usu\u00e1rio, revers\u00edvel \u2014 detalhes: docs/avvio-al-login.md)",
    L"Shell atual: %s\r\nPID %lu \u2014 %s",
    L"Destino: %s\r\n%s",
    L"O Explorer ser\u00e1 reiniciado.\r\nO trabalho n\u00e3o salvo poder\u00e1 ser "
    L"perdido.\r\n\r\nContinuar?",
    L"Execut\u00e1vel de destino n\u00e3o encontrado:\r\n%s\r\n\r\nNada foi alternado.",
    L"Recusa em parar a shell atual: seu execut\u00e1vel\r\nn\u00e3o \u00e9 um explorer "
    L"reconhecido:\r\n%s\r\n\r\nNada foi alternado.",
    L"A shell selecionada j\u00e1 est\u00e1 em execu\u00e7\u00e3o.",
    L"Falha ao iniciar o Windows 7 Explorer:\r\n%s\r\nErro %lu de "
    L"CreateProcess.\r\n\r\nTentando restaurar a shell nativa\u2026",
    L"CR\u00cdTICO: n\u00e3o foi poss\u00edvel iniciar NENHUMA shell.\r\nA restaura\u00e7\u00e3o nativa "
    L"tamb\u00e9m falhou (erro %lu).\r\n\r\nRecupera\u00e7\u00e3o: Ctrl+Alt+Shift+S abre este "
    L"alternador; ou Ctrl+Shift+Esc \u2192 Gerenciador de Tarefas \u2192 Executar nova "
    L"tarefa \u2192 %s",
    L"Falha ao iniciar a shell nativa:\r\n%s\r\nErro %lu de CreateProcess.\r\n\r\nTentar "
    L"iniciar %s de novo?",
    L"N\u00e3o foi poss\u00edvel %ls a inicializa\u00e7\u00e3o autom\u00e1tica ao entrar (erro %lu).\r\nNada "
    L"mais foi alterado.",
    L"O Windows 7 Explorer ser\u00e1 iniciado ao entrar. Aplicado (tudo "
    L"revers\u00edvel):\r\n\u2022 valor Shell do usu\u00e1rio (HKCU ...\\Winlogon\\Shell) \u2014 valor "
    L"anterior salvo;\r\n\u2022 link na pasta Inicializar (reserva + rein\u00edcio por tecla "
    L"de atalho);\r\n\u2022 tarefa de recupera\u00e7\u00e3o \"7explorer Shell Recovery\" (~30 s ap\u00f3s "
    L"a entrada:\r\n   se a shell privada n\u00e3o estiver ativa, restaura "
    L"tudo).\r\n\r\nDesmarque a caixa para desfazer as tr\u00eas coisas (o valor anterior "
    L"\u00e9 restaurado\r\nbyte por byte). Detalhes: docs/avvio-al-login.md",
    L"Selecione o Win7ExplorerRestorer privado (explorer.exe)",
    L"explorer.exe\0explorer.exe\0Todos os arquivos\0*.*\0",
    L"A opera\u00e7\u00e3o do link em Inicializar falhou (erro %lu).",
    L"Padr\u00e3o do sistema",
    L"English",
    L"Italiano",
    L"Personalizar tema\u2026",
    L"Selecione O SEU arquivo de tema do Windows 7 (aero.msstyles)",
    L"Arquivos de tema\0*.msstyles\0Todos os arquivos\0*.*\0",
    L"Tema instalado em:\r\n%s\r\n\r\nAlterne a shell (ex.: nativa \u2192 "
    L"Win7ExplorerRestorer) para aplic\u00e1-lo.\r\nO arquivo \u00e9 SEU: foi apenas copiado "
    L"localmente, nada baixado ou compartilhado.",
    L"N\u00e3o foi poss\u00edvel copiar o arquivo de tema (erro %lu):\r\n%s",
    L"N\u00e3o \u00e9 poss\u00edvel ativar a inicializa\u00e7\u00e3o autom\u00e1tica ao entrar:\r\n%s\r\n\r\nNessa "
    L"pasta devem existir explorer.exe e wrp64.dll\r\n(o valor Shell nunca aponta "
    L"para arquivos ausentes).",
    L"Inicializa\u00e7\u00e3o autom\u00e1tica removida.\r\nO valor Shell anterior foi restaurado e "
    L"o link de reserva\r\ne a tarefa de recupera\u00e7\u00e3o foram exclu\u00eddos.",
    L"O valor Shell do usu\u00e1rio foi definido, mas a tarefa de recupera\u00e7\u00e3o\r\nn\u00e3o "
    L"p\u00f4de ser registrada (erro %lu).\r\nA inicializa\u00e7\u00e3o autom\u00e1tica funciona; s\u00f3 "
    L"falta a rede de seguran\u00e7a\r\nautom\u00e1tica ao entrar (detalhes: "
    L"docs/avvio-al-login.md).",
    L"O Windows 7 Explorer Restorer ainda n\u00e3o est\u00e1 instalado",
    L"Para us\u00e1-lo, \u00e9 preciso baixar um arquivo da Microsoft, verific\u00e1-lo e "
    L"prepar\u00e1-lo. A conex\u00e3o com a Internet s\u00f3 \u00e9 necess\u00e1ria na primeira vez.",
    L"Reinstalar o Windows 7 Explorer Restorer?",
    L"A c\u00f3pia privada do explorer.exe ser\u00e1 baixada e preparada de novo. Se "
    L"estiver em uso, voc\u00ea voltar\u00e1 antes ao Explorador de Arquivos.",
    L"Instalar",
    L"Reinstalar",
    L"Interromper",
    L"N\u00e3o s\u00e3o necess\u00e1rios direitos de administrador.",
    L"\u00c9 necess\u00e1rio cerca de um minuto.",
    L"Instala\u00e7\u00e3o em andamento\u2026",
    L"Instala\u00e7\u00e3o falhou (c\u00f3digo %lu).\r\n\r\n%s",
    L"Instala\u00e7\u00e3o falhou \u2014 ver detalhes.",
    L"Instala\u00e7\u00e3o interrompida.",
    L"O instalador n\u00e3o foi encontrado:\r\n%s\r\n\r\nCopie Win7ExplorerRestorer.exe para "
    L"junto deste alternador (mesma pasta) e tente de novo.",
    L"Instalador n\u00e3o encontrado.",
    L"N\u00e3o foi poss\u00edvel iniciar o instalador (erro %lu):\r\n%s",
    L"Escolha qual Explorer usar como shell do Windows. A altern\u00e2ncia \u00e9 imediata "
    L"e n\u00e3o exige sair.",
    L"Shell padr\u00e3o do sistema",
    L"  \u2013 em uso",
    L"Usar o Windows 7 Explorer Restorer a cada entrada",
    L"Mais informa\u00e7\u00f5es",
    L"Reinstalar",
    L"Desinstalar",
    L"Idioma",
    L"Usar Win7ExplorerRestorer",
    L"Usar o Explorer nativo",
    L"A \u00e1rea de trabalho ser\u00e1 reiniciada brevemente.",
    L"A inicializa\u00e7\u00e3o autom\u00e1tica define o valor Shell do usu\u00e1rio (HKCU) para o "
    L"explorer.exe privado \u2014 o jeito padr\u00e3o do Windows, sem eleva\u00e7\u00e3o, totalmente "
    L"revers\u00edvel (o valor anterior \u00e9 salvo e restaurado byte por byte).\r\n\r\nTamb\u00e9m "
    L"adiciona duas redes de seguran\u00e7a: um link na pasta Inicializar e uma tarefa "
    L"agendada (\"7explorer Shell Recovery\") que verifica a shell ~30 s ap\u00f3s a "
    L"entrada.\r\n\r\nDesmarque a caixa para desfazer tudo.",
    L"Reinstala\u00e7\u00e3o conclu\u00edda.\r\n\r\nVoltar agora ao Windows 7 Explorer Restorer?",
    L"O Windows 7 Explorer Restorer ser\u00e1 removido: voc\u00ea voltar\u00e1 ao Explorador de "
    L"Arquivos e os arquivos privados ser\u00e3o exclu\u00eddos.\r\n\r\nProsseguir?",
    L"O Windows 7 Explorer Restorer foi removido.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_RU[] = {
    L"\u041f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0430\u0435\u0442 \u0440\u0430\u0431\u043e\u0442\u0430\u044e\u0449\u0443\u044e \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443 Explorer \u043d\u0430 \u043b\u0435\u0442\u0443. \u0412\u044b\u0445\u043e\u0434 \u0438\u0437 \u0441\u0438\u0441\u0442\u0435\u043c\u044b \u043d\u0435 "
    L"\u043d\u0443\u0436\u0435\u043d.",
    L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443 Explorer",
    L"\u0421\u0442\u0430\u043d\u0434\u0430\u0440\u0442\u043d\u044b\u0439 \u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a Windows",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(\u043d\u0435 \u043e\u0431\u043d\u0430\u0440\u0443\u0436\u0435\u043d\u0430)",
    L"\u041d\u0435\u0438\u0437\u0432\u0435\u0441\u0442\u043d\u044b\u0439 \u043f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a (\u0441\u043c. \u043f\u0443\u0442\u044c)",
    L"\u041f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0438\u0442\u044c",
    L"\u041e\u0442\u043c\u0435\u043d\u0430",
    L"\u041e\u0431\u0437\u043e\u0440\u2026",
    L"\u0417\u0430\u043f\u0443\u0441\u043a\u0430\u0442\u044c Windows 7 Explorer \u0430\u0432\u0442\u043e\u043c\u0430\u0442\u0438\u0447\u0435\u0441\u043a\u0438 \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435\r\n(\u0437\u0430\u0434\u0430\u0451\u0442 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 "
    L"Shell \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f, \u043e\u0431\u0440\u0430\u0442\u0438\u043c\u043e \u2014 \u043f\u043e\u0434\u0440\u043e\u0431\u043d\u043e\u0441\u0442\u0438: docs/avvio-al-login.md)",
    L"\u0422\u0435\u043a\u0443\u0449\u0430\u044f \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0430: %s\r\nPID %lu \u2014 %s",
    L"\u0426\u0435\u043b\u044c: %s\r\n%s",
    L"\u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a \u0431\u0443\u0434\u0435\u0442 \u043f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0449\u0435\u043d.\r\n\u041d\u0435\u0441\u043e\u0445\u0440\u0430\u043d\u0451\u043d\u043d\u0430\u044f \u0440\u0430\u0431\u043e\u0442\u0430 \u043c\u043e\u0436\u0435\u0442 \u0431\u044b\u0442\u044c "
    L"\u043f\u043e\u0442\u0435\u0440\u044f\u043d\u0430.\r\n\r\n\u041f\u0440\u043e\u0434\u043e\u043b\u0436\u0438\u0442\u044c?",
    L"\u0426\u0435\u043b\u0435\u0432\u043e\u0439 \u0438\u0441\u043f\u043e\u043b\u043d\u044f\u0435\u043c\u044b\u0439 \u0444\u0430\u0439\u043b \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d:\r\n%s\r\n\r\n\u041d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 \u043f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0435\u043d\u043e.",
    L"\u041e\u0442\u043a\u0430\u0437 \u043e\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0442\u044c \u0442\u0435\u043a\u0443\u0449\u0443\u044e \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443: \u0435\u0451 \u0438\u0441\u043f\u043e\u043b\u043d\u044f\u0435\u043c\u044b\u0439 \u0444\u0430\u0439\u043b\r\n\u043d\u0435 \u044f\u0432\u043b\u044f\u0435\u0442\u0441\u044f "
    L"\u0440\u0430\u0441\u043f\u043e\u0437\u043d\u0430\u043d\u043d\u044b\u043c \u043f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a\u043e\u043c:\r\n%s\r\n\r\n\u041d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 \u043f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0435\u043d\u043e.",
    L"\u0412\u044b\u0431\u0440\u0430\u043d\u043d\u0430\u044f \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0430 \u0443\u0436\u0435 \u0437\u0430\u043f\u0443\u0449\u0435\u043d\u0430.",
    L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c Windows 7 Explorer:\r\n%s\r\n\u041e\u0448\u0438\u0431\u043a\u0430 CreateProcess "
    L"%lu.\r\n\r\n\u041f\u043e\u043f\u044b\u0442\u043a\u0430 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c \u0448\u0442\u0430\u0442\u043d\u0443\u044e \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443\u2026",
    L"\u041a\u0420\u0418\u0422\u0418\u0427\u0415\u0421\u041a\u0418: \u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u041d\u0418 \u041e\u0414\u041d\u0423 \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443.\r\n\u0428\u0442\u0430\u0442\u043d\u043e\u0435 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0435 "
    L"\u0442\u043e\u0436\u0435 \u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c (\u043e\u0448\u0438\u0431\u043a\u0430 %lu).\r\n\r\n\u0412\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u0435: Ctrl+Alt+Shift+S \u043e\u0442\u043a\u0440\u044b\u0432\u0430\u0435\u0442 "
    L"\u044d\u0442\u043e\u0442 \u043f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0430\u0442\u0435\u043b\u044c; \u0438\u043b\u0438 Ctrl+Shift+Esc \u2192 \u0414\u0438\u0441\u043f\u0435\u0442\u0447\u0435\u0440 \u0437\u0430\u0434\u0430\u0447 \u2192 \u0417\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u043d\u043e\u0432\u0443\u044e "
    L"\u0437\u0430\u0434\u0430\u0447\u0443 \u2192 %s",
    L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u0448\u0442\u0430\u0442\u043d\u0443\u044e \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443:\r\n%s\r\n\u041e\u0448\u0438\u0431\u043a\u0430 CreateProcess "
    L"%lu.\r\n\r\n\u041f\u043e\u0432\u0442\u043e\u0440\u0438\u0442\u044c \u0437\u0430\u043f\u0443\u0441\u043a %s?",
    L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c %ls \u0430\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435 (\u043e\u0448\u0438\u0431\u043a\u0430 %lu).\r\n\u0411\u043e\u043b\u044c\u0448\u0435 \u043d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 "
    L"\u0438\u0437\u043c\u0435\u043d\u0435\u043d\u043e.",
    L"Windows 7 Explorer \u0431\u0443\u0434\u0435\u0442 \u0437\u0430\u043f\u0443\u0441\u043a\u0430\u0442\u044c\u0441\u044f \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435. \u041f\u0440\u0438\u043c\u0435\u043d\u0435\u043d\u043e (\u0432\u0441\u0451 "
    L"\u043e\u0431\u0440\u0430\u0442\u0438\u043c\u043e):\r\n\u2022 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 Shell \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f (HKCU ...\\Winlogon\\Shell) \u2014 "
    L"\u043f\u0440\u0435\u0436\u043d\u0435\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e;\r\n\u2022 \u044f\u0440\u043b\u044b\u043a \u0432 \u043f\u0430\u043f\u043a\u0435 \u00ab\u0410\u0432\u0442\u043e\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430\u00bb (\u0437\u0430\u043f\u0430\u0441\u043d\u043e\u0439 + "
    L"\u043f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0441\u043a \u0433\u043e\u0440\u044f\u0447\u0435\u0439 \u043a\u043b\u0430\u0432\u0438\u0448\u0435\u0439);\r\n\u2022 \u0437\u0430\u0434\u0430\u0447\u0430 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \"7explorer Shell "
    L"Recovery\" (~30 \u0441 \u043f\u043e\u0441\u043b\u0435 \u0432\u0445\u043e\u0434\u0430:\r\n   \u0435\u0441\u043b\u0438 \u0447\u0430\u0441\u0442\u043d\u0430\u044f \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0430 \u043d\u0435 \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442, "
    L"\u0432\u043e\u0441\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0435\u0442 \u0432\u0441\u0451).\r\n\r\n\u0421\u043d\u0438\u043c\u0438\u0442\u0435 \u0444\u043b\u0430\u0436\u043e\u043a, \u0447\u0442\u043e\u0431\u044b \u043e\u0442\u043c\u0435\u043d\u0438\u0442\u044c \u0432\u0441\u0435 \u0442\u0440\u0438 \u0434\u0435\u0439\u0441\u0442\u0432\u0438\u044f "
    L"(\u043f\u0440\u0435\u0436\u043d\u0435\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 \u0431\u0443\u0434\u0435\u0442\r\n\u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043e \u043f\u043e\u0431\u0430\u0439\u0442\u043e\u0432\u043e). \u041f\u043e\u0434\u0440\u043e\u0431\u043d\u043e\u0441\u0442\u0438: "
    L"docs/avvio-al-login.md",
    L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0447\u0430\u0441\u0442\u043d\u044b\u0439 Win7ExplorerRestorer (explorer.exe)",
    L"explorer.exe\0explorer.exe\0\u0412\u0441\u0435 \u0444\u0430\u0439\u043b\u044b\0*.*\0",
    L"\u041e\u043f\u0435\u0440\u0430\u0446\u0438\u044f \u0441 \u044f\u0440\u043b\u044b\u043a\u043e\u043c \u0432 \u00ab\u0410\u0432\u0442\u043e\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0435\u00bb \u043d\u0435 \u0443\u0434\u0430\u043b\u0430\u0441\u044c (\u043e\u0448\u0438\u0431\u043a\u0430 %lu).",
    L"\u0421\u0438\u0441\u0442\u0435\u043c\u043d\u043e\u0435 \u043f\u043e \u0443\u043c\u043e\u043b\u0447\u0430\u043d\u0438\u044e",
    L"English",
    L"Italiano",
    L"\u041d\u0430\u0441\u0442\u0440\u043e\u0438\u0442\u044c \u0442\u0435\u043c\u0443\u2026",
    L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u0412\u0410\u0428 \u0444\u0430\u0439\u043b \u0442\u0435\u043c\u044b Windows 7 (aero.msstyles)",
    L"\u0424\u0430\u0439\u043b\u044b \u0442\u0435\u043c\0*.msstyles\0\u0412\u0441\u0435 \u0444\u0430\u0439\u043b\u044b\0*.*\0",
    L"\u0422\u0435\u043c\u0430 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0430 \u0432:\r\n%s\r\n\r\n\u041f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0438\u0442\u0435 \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443 (\u043d\u0430\u043f\u0440. \u0448\u0442\u0430\u0442\u043d\u0430\u044f \u2192 "
    L"Win7ExplorerRestorer), \u0447\u0442\u043e\u0431\u044b \u043f\u0440\u0438\u043c\u0435\u043d\u0438\u0442\u044c \u0435\u0451.\r\n\u0424\u0430\u0439\u043b \u0412\u0410\u0428: \u043e\u043d \u0431\u044b\u043b \u0442\u043e\u043b\u044c\u043a\u043e "
    L"\u0441\u043a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u043d \u043b\u043e\u043a\u0430\u043b\u044c\u043d\u043e, \u043d\u0438\u0447\u0435\u0433\u043e \u043d\u0435 \u0441\u043a\u0430\u0447\u0430\u043d\u043e \u0438 \u043d\u0435 \u043f\u0435\u0440\u0435\u0434\u0430\u043d\u043e.",
    L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0441\u043a\u043e\u043f\u0438\u0440\u043e\u0432\u0430\u0442\u044c \u0444\u0430\u0439\u043b \u0442\u0435\u043c\u044b (\u043e\u0448\u0438\u0431\u043a\u0430 %lu):\r\n%s",
    L"\u041d\u0435\u0432\u043e\u0437\u043c\u043e\u0436\u043d\u043e \u0432\u043a\u043b\u044e\u0447\u0438\u0442\u044c \u0430\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435:\r\n%s\r\n\r\n\u0412 \u044d\u0442\u043e\u0439 \u043f\u0430\u043f\u043a\u0435 \u0434\u043e\u043b\u0436\u043d\u044b \u0431\u044b\u0442\u044c "
    L"explorer.exe \u0438 wrp64.dll\r\n(\u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 Shell \u043d\u0438\u043a\u043e\u0433\u0434\u0430 \u043d\u0435 \u0443\u043a\u0430\u0437\u044b\u0432\u0430\u0435\u0442 \u043d\u0430 "
    L"\u043e\u0442\u0441\u0443\u0442\u0441\u0442\u0432\u0443\u044e\u0449\u0438\u0435 \u0444\u0430\u0439\u043b\u044b).",
    L"\u0410\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435 \u0443\u0434\u0430\u043b\u0451\u043d.\r\n\u041f\u0440\u0435\u0436\u043d\u0435\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 Shell \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u043e, "
    L"\u0437\u0430\u043f\u0430\u0441\u043d\u043e\u0439 \u044f\u0440\u043b\u044b\u043a\r\n\u0438 \u0437\u0430\u0434\u0430\u0447\u0430 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f \u0443\u0434\u0430\u043b\u0435\u043d\u044b.",
    L"\u0417\u043d\u0430\u0447\u0435\u043d\u0438\u0435 Shell \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f \u0437\u0430\u0434\u0430\u043d\u043e, \u043d\u043e \u0437\u0430\u0434\u0430\u0447\u0443 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d\u0438\u044f\r\n\u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c "
    L"\u0437\u0430\u0440\u0435\u0433\u0438\u0441\u0442\u0440\u0438\u0440\u043e\u0432\u0430\u0442\u044c (\u043e\u0448\u0438\u0431\u043a\u0430 %lu).\r\n\u0410\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442; \u043e\u0442\u0441\u0443\u0442\u0441\u0442\u0432\u0443\u0435\u0442 \u0442\u043e\u043b\u044c\u043a\u043e "
    L"\u0430\u0432\u0442\u043e\u043c\u0430\u0442\u0438\u0447\u0435\u0441\u043a\u0430\u044f\r\n\u0441\u0442\u0440\u0430\u0445\u043e\u0432\u043a\u0430 \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435 (\u043f\u043e\u0434\u0440\u043e\u0431\u043d\u043e\u0441\u0442\u0438: docs/avvio-al-login.md).",
    L"Windows 7 Explorer Restorer \u043f\u043e\u043a\u0430 \u043d\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043b\u0435\u043d",
    L"\u0414\u043b\u044f \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u044f \u043d\u0443\u0436\u043d\u043e \u0441\u043a\u0430\u0447\u0430\u0442\u044c \u0444\u0430\u0439\u043b \u0441 \u0441\u0430\u0439\u0442\u0430 Microsoft, \u043f\u0440\u043e\u0432\u0435\u0440\u0438\u0442\u044c \u0438 "
    L"\u043f\u043e\u0434\u0433\u043e\u0442\u043e\u0432\u0438\u0442\u044c \u0435\u0433\u043e. \u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u043a \u0418\u043d\u0442\u0435\u0440\u043d\u0435\u0442\u0443 \u043d\u0443\u0436\u043d\u043e \u0442\u043e\u043b\u044c\u043a\u043e \u0432 \u043f\u0435\u0440\u0432\u044b\u0439 \u0440\u0430\u0437.",
    L"\u041f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c Windows 7 Explorer Restorer?",
    L"\u0427\u0430\u0441\u0442\u043d\u0430\u044f \u043a\u043e\u043f\u0438\u044f explorer.exe \u0431\u0443\u0434\u0435\u0442 \u0441\u043a\u0430\u0447\u0430\u043d\u0430 \u0438 \u043f\u043e\u0434\u0433\u043e\u0442\u043e\u0432\u043b\u0435\u043d\u0430 \u0437\u0430\u043d\u043e\u0432\u043e. \u0415\u0441\u043b\u0438 \u043e\u043d\u0430 "
    L"\u0438\u0441\u043f\u043e\u043b\u044c\u0437\u0443\u0435\u0442\u0441\u044f, \u0441\u043d\u0430\u0447\u0430\u043b\u0430 \u043f\u0440\u043e\u0438\u0437\u043e\u0439\u0434\u0451\u0442 \u0432\u043e\u0437\u0432\u0440\u0430\u0442 \u043a \u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a\u0443 Windows.",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c",
    L"\u041f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c",
    L"\u041f\u0440\u0435\u0440\u0432\u0430\u0442\u044c",
    L"\u041f\u0440\u0430\u0432\u0430 \u0430\u0434\u043c\u0438\u043d\u0438\u0441\u0442\u0440\u0430\u0442\u043e\u0440\u0430 \u043d\u0435 \u043d\u0443\u0436\u043d\u044b.",
    L"\u041f\u043e\u0442\u0440\u0435\u0431\u0443\u0435\u0442\u0441\u044f \u043e\u043a\u043e\u043b\u043e \u043c\u0438\u043d\u0443\u0442\u044b.",
    L"\u0412\u044b\u043f\u043e\u043b\u043d\u044f\u0435\u0442\u0441\u044f \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430\u2026",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u043d\u0435 \u0443\u0434\u0430\u043b\u0430\u0441\u044c (\u043a\u043e\u0434 %lu).\r\n\r\n%s",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u043d\u0435 \u0443\u0434\u0430\u043b\u0430\u0441\u044c \u2014 \u0441\u043c. \u043f\u043e\u0434\u0440\u043e\u0431\u043d\u043e\u0441\u0442\u0438.",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u043f\u0440\u0435\u0440\u0432\u0430\u043d\u0430.",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0449\u0438\u043a \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d:\r\n%s\r\n\r\n\u0421\u043a\u043e\u043f\u0438\u0440\u0443\u0439\u0442\u0435 Win7ExplorerRestorer.exe \u0440\u044f\u0434\u043e\u043c \u0441 "
    L"\u044d\u0442\u0438\u043c \u043f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0430\u0442\u0435\u043b\u0435\u043c (\u0432 \u0442\u0443 \u0436\u0435 \u043f\u0430\u043f\u043a\u0443) \u0438 \u043f\u043e\u0432\u0442\u043e\u0440\u0438\u0442\u0435.",
    L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0449\u0438\u043a \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d.",
    L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u0437\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0449\u0438\u043a (\u043e\u0448\u0438\u0431\u043a\u0430 %lu):\r\n%s",
    L"\u0412\u044b\u0431\u0435\u0440\u0438\u0442\u0435 \u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a \u0434\u043b\u044f \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u043d\u0438\u044f \u0432 \u043a\u0430\u0447\u0435\u0441\u0442\u0432\u0435 \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0438 Windows. "
    L"\u041f\u0435\u0440\u0435\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u043c\u0433\u043d\u043e\u0432\u0435\u043d\u043d\u043e\u0435, \u0432\u044b\u0445\u043e\u0434 \u043d\u0435 \u043d\u0443\u0436\u0435\u043d.",
    L"\u0428\u0442\u0430\u0442\u043d\u0430\u044f \u0441\u0438\u0441\u0442\u0435\u043c\u043d\u0430\u044f \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0430",
    L"  \u2013 \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u0443\u0435\u0442\u0441\u044f",
    L"\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c Windows 7 Explorer Restorer \u043f\u0440\u0438 \u043a\u0430\u0436\u0434\u043e\u043c \u0432\u0445\u043e\u0434\u0435",
    L"\u041f\u043e\u0434\u0440\u043e\u0431\u043d\u0435\u0435",
    L"\u041f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c",
    L"\u0423\u0434\u0430\u043b\u0438\u0442\u044c",
    L"\u042f\u0437\u044b\u043a",
    L"\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c Win7ExplorerRestorer",
    L"\u0418\u0441\u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u044c \u0448\u0442\u0430\u0442\u043d\u044b\u0439 \u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a",
    L"\u0420\u0430\u0431\u043e\u0447\u0438\u0439 \u0441\u0442\u043e\u043b \u0431\u0443\u0434\u0435\u0442 \u043d\u0435\u043d\u0430\u0434\u043e\u043b\u0433\u043e \u043f\u0435\u0440\u0435\u0437\u0430\u043f\u0443\u0449\u0435\u043d.",
    L"\u0410\u0432\u0442\u043e\u0437\u0430\u043f\u0443\u0441\u043a \u043f\u0440\u0438 \u0432\u0445\u043e\u0434\u0435 \u0437\u0430\u0434\u0430\u0451\u0442 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 Shell \u043f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044f (HKCU) \u043d\u0430 \u0447\u0430\u0441\u0442\u043d\u044b\u0439 "
    L"explorer.exe \u2014 \u0441\u0442\u0430\u043d\u0434\u0430\u0440\u0442\u043d\u044b\u0439 \u0441\u043f\u043e\u0441\u043e\u0431 Windows, \u0431\u0435\u0437 \u043f\u043e\u0432\u044b\u0448\u0435\u043d\u0438\u044f \u043f\u0440\u0430\u0432, \u043f\u043e\u043b\u043d\u043e\u0441\u0442\u044c\u044e "
    L"\u043e\u0431\u0440\u0430\u0442\u0438\u043c\u043e (\u043f\u0440\u0435\u0436\u043d\u0435\u0435 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0435 \u0441\u043e\u0445\u0440\u0430\u043d\u044f\u0435\u0442\u0441\u044f \u0438 \u0432\u043e\u0441\u0441\u0442\u0430\u043d\u0430\u0432\u043b\u0438\u0432\u0430\u0435\u0442\u0441\u044f "
    L"\u043f\u043e\u0431\u0430\u0439\u0442\u043e\u0432\u043e).\r\n\r\n\u0422\u0430\u043a\u0436\u0435 \u0434\u043e\u0431\u0430\u0432\u043b\u044f\u044e\u0442\u0441\u044f \u0434\u0432\u0435 \u0441\u0442\u0440\u0430\u0445\u043e\u0432\u043a\u0438: \u044f\u0440\u043b\u044b\u043a \u0432 \u043f\u0430\u043f\u043a\u0435 "
    L"\u00ab\u0410\u0432\u0442\u043e\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430\u00bb \u0438 \u0437\u0430\u043f\u043b\u0430\u043d\u0438\u0440\u043e\u0432\u0430\u043d\u043d\u0430\u044f \u0437\u0430\u0434\u0430\u0447\u0430 (\"7explorer Shell Recovery\"), "
    L"\u043f\u0440\u043e\u0432\u0435\u0440\u044f\u044e\u0449\u0430\u044f \u043e\u0431\u043e\u043b\u043e\u0447\u043a\u0443 ~30 \u0441 \u043f\u043e\u0441\u043b\u0435 \u0432\u0445\u043e\u0434\u0430.\r\n\r\n\u0421\u043d\u0438\u043c\u0438\u0442\u0435 \u0444\u043b\u0430\u0436\u043e\u043a, \u0447\u0442\u043e\u0431\u044b \u043e\u0442\u043c\u0435\u043d\u0438\u0442\u044c "
    L"\u0432\u0441\u0451.",
    L"\u041f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0430 \u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043d\u0430.\r\n\r\n\u0412\u0435\u0440\u043d\u0443\u0442\u044c\u0441\u044f \u0441\u0435\u0439\u0447\u0430\u0441 \u043a Windows 7 Explorer Restorer?",
    L"Windows 7 Explorer Restorer \u0431\u0443\u0434\u0435\u0442 \u0443\u0434\u0430\u043b\u0451\u043d: \u043f\u0440\u043e\u0438\u0437\u043e\u0439\u0434\u0451\u0442 \u0432\u043e\u0437\u0432\u0440\u0430\u0442 \u043a \u041f\u0440\u043e\u0432\u043e\u0434\u043d\u0438\u043a\u0443 "
    L"Windows, \u0447\u0430\u0441\u0442\u043d\u044b\u0435 \u0444\u0430\u0439\u043b\u044b \u0431\u0443\u0434\u0443\u0442 \u0443\u0434\u0430\u043b\u0435\u043d\u044b.\r\n\r\n\u041f\u0440\u043e\u0434\u043e\u043b\u0436\u0438\u0442\u044c?",
    L"Windows 7 Explorer Restorer \u0443\u0434\u0430\u043b\u0451\u043d.",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR* TR_ZHCN[] = {
    L"\u5373\u65f6\u5207\u6362\u6b63\u5728\u8fd0\u884c\u7684 Explorer \u5916\u58f3\u3002\u65e0\u9700\u6ce8\u9500\u3002",
    L"\u9009\u62e9 Explorer \u5916\u58f3",
    L"\u539f\u751f Windows \u8d44\u6e90\u7ba1\u7406\u5668",
    L"Win7ExplorerRestorer (Windows 7)",
    L"(\u672a\u68c0\u6d4b\u5230)",
    L"\u672a\u77e5\u7684\u8d44\u6e90\u7ba1\u7406\u5668(\u89c1\u8def\u5f84)",
    L"\u5207\u6362",
    L"\u53d6\u6d88",
    L"\u6d4f\u89c8\u2026",
    L"\u5728\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8 Windows 7 Explorer\r\n(\u8bbe\u7f6e\u6309\u7528\u6237\u7684 Shell \u503c\uff0c\u53ef\u8fd8\u539f \u2014 \u8be6\u60c5: "
    L"docs/avvio-al-login.md)",
    L"\u5f53\u524d\u5916\u58f3: %s\r\nPID %lu \u2014 %s",
    L"\u76ee\u6807: %s\r\n%s",
    L"\u8d44\u6e90\u7ba1\u7406\u5668\u5c06\u91cd\u65b0\u542f\u52a8\u3002\r\n\u672a\u4fdd\u5b58\u7684\u5de5\u4f5c\u53ef\u80fd\u4f1a\u4e22\u5931\u3002\r\n\r\n\u7ee7\u7eed\u5417?",
    L"\u627e\u4e0d\u5230\u76ee\u6807\u53ef\u6267\u884c\u6587\u4ef6:\r\n%s\r\n\r\n\u672a\u5207\u6362\u4efb\u4f55\u5185\u5bb9\u3002",
    L"\u62d2\u7edd\u505c\u6b62\u5f53\u524d\u5916\u58f3: \u5176\u53ef\u6267\u884c\u6587\u4ef6\r\n\u4e0d\u662f\u53ef\u8bc6\u522b\u7684 explorer:\r\n%s\r\n\r\n\u672a\u5207\u6362\u4efb\u4f55\u5185\u5bb9\u3002",
    L"\u6240\u9009\u5916\u58f3\u5df2\u5728\u8fd0\u884c\u3002",
    L"\u65e0\u6cd5\u542f\u52a8 Windows 7 Explorer:\r\n%s\r\nCreateProcess \u9519\u8bef %lu\u3002\r\n\r\n\u6b63\u5728\u5c1d\u8bd5\u8fd8\u539f\u539f\u751f\u5916\u58f3\u2026",
    L"\u4e25\u91cd: \u65e0\u6cd5\u542f\u52a8\u4efb\u4f55\u5916\u58f3\u3002\r\n\u539f\u751f\u8fd8\u539f\u4e5f\u5931\u8d25\u4e86(\u9519\u8bef %lu)\u3002\r\n\r\n\u6062\u590d: Ctrl+Alt+Shift+S \u6253\u5f00\u6b64\u5207\u6362\u5668\uff1b\u6216 "
    L"Ctrl+Shift+Esc \u2192 \u4efb\u52a1\u7ba1\u7406\u5668 \u2192 \u8fd0\u884c\u65b0\u4efb\u52a1 \u2192 %s",
    L"\u65e0\u6cd5\u542f\u52a8\u539f\u751f\u5916\u58f3:\r\n%s\r\nCreateProcess \u9519\u8bef %lu\u3002\r\n\r\n\u91cd\u8bd5\u542f\u52a8 %s \u5417?",
    L"\u65e0\u6cd5%ls\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8(\u9519\u8bef %lu)\u3002\r\n\u672a\u66f4\u6539\u5176\u4ed6\u5185\u5bb9\u3002",
    L"Windows 7 Explorer \u5c06\u5728\u767b\u5f55\u65f6\u542f\u52a8\u3002\u5df2\u5e94\u7528(\u5168\u90e8\u53ef\u8fd8\u539f):\r\n\u2022 \u6309\u7528\u6237\u7684 Shell \u503c(HKCU "
    L"...\\Winlogon\\Shell) \u2014 \u5df2\u4fdd\u5b58\u4ee5\u524d\u7684\u503c\uff1b\r\n\u2022 \u542f\u52a8\u6587\u4ef6\u5939\u4e2d\u7684\u94fe\u63a5(\u5907\u7528 + \u70ed\u952e\u91cd\u542f)\uff1b\r\n\u2022 \u6062\u590d\u4efb\u52a1 \"7explorer "
    L"Shell Recovery\"(\u767b\u5f55\u540e\u7ea6 30 \u79d2:\r\n   \u5982\u679c\u79c1\u6709\u5916\u58f3\u672a\u8fd0\u884c\uff0c\u5219\u8fd8\u539f\u4e00\u5207)\u3002\r\n\r\n\u53d6\u6d88\u9009\u4e2d\u8be5\u590d\u9009\u6846\u53ef\u64a4\u9500\u8fd9\u4e09\u9879(\u4ee5\u524d\u7684\u503c\u5c06\u6309\r\n\u5b57"
    L"\u8282\u539f\u6837\u8fd8\u539f)\u3002\u8be6\u60c5: docs/avvio-al-login.md",
    L"\u9009\u62e9\u79c1\u6709\u7684 Win7ExplorerRestorer (explorer.exe)",
    L"explorer.exe\0explorer.exe\0\u6240\u6709\u6587\u4ef6\0*.*\0",
    L"\u542f\u52a8\u6587\u4ef6\u5939\u94fe\u63a5\u64cd\u4f5c\u5931\u8d25(\u9519\u8bef %lu)\u3002",
    L"\u7cfb\u7edf\u9ed8\u8ba4",
    L"English",
    L"Italiano",
    L"\u81ea\u5b9a\u4e49\u4e3b\u9898\u2026",
    L"\u9009\u62e9\u4f60\u7684 Windows 7 \u4e3b\u9898\u6587\u4ef6 (aero.msstyles)",
    L"\u4e3b\u9898\u6587\u4ef6\0*.msstyles\0\u6240\u6709\u6587\u4ef6\0*.*\0",
    L"\u4e3b\u9898\u5df2\u5b89\u88c5\u5230:\r\n%s\r\n\r\n\u5207\u6362\u5916\u58f3(\u4f8b\u5982 \u539f\u751f \u2192 Win7ExplorerRestorer)\u4ee5\u5e94\u7528\u3002\r\n\u8be5\u6587\u4ef6\u5c5e\u4e8e\u4f60: "
    L"\u4ec5\u5728\u672c\u5730\u590d\u5236\uff0c\u672a\u4e0b\u8f7d\u6216\u5171\u4eab\u4efb\u4f55\u5185\u5bb9\u3002",
    L"\u65e0\u6cd5\u590d\u5236\u4e3b\u9898\u6587\u4ef6(\u9519\u8bef %lu):\r\n%s",
    L"\u65e0\u6cd5\u542f\u7528\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8:\r\n%s\r\n\r\n\u8be5\u6587\u4ef6\u5939\u4e2d\u5fc5\u987b\u540c\u65f6\u5b58\u5728 explorer.exe \u548c wrp64.dll\r\n(Shell "
    L"\u503c\u4ece\u4e0d\u6307\u5411\u7f3a\u5931\u7684\u6587\u4ef6)\u3002",
    L"\u5df2\u5220\u9664\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8\u3002\r\n\u5df2\u8fd8\u539f\u4ee5\u524d\u7684 Shell \u503c\uff0c\u5e76\u5220\u9664\u4e86\u5907\u7528\u94fe\u63a5\r\n\u548c\u6062\u590d\u4efb\u52a1\u3002",
    L"\u5df2\u8bbe\u7f6e\u6309\u7528\u6237\u7684 Shell \u503c\uff0c\u4f46\u65e0\u6cd5\u6ce8\u518c\u6062\u590d\u4efb\u52a1\r\n(\u9519\u8bef %lu)\u3002\r\n\u81ea\u52a8\u542f\u52a8\u4ecd\u53ef\u5de5\u4f5c\uff1b\u53ea\u662f\u5728\u767b\u5f55\u65f6\u6ca1\u6709\u81ea\u52a8\u5b89\u5168\u7f51\r\n(\u8be6\u60c5: "
    L"docs/avvio-al-login.md)\u3002",
    L"\u5c1a\u672a\u5b89\u88c5 Windows 7 Explorer Restorer",
    L"\u82e5\u8981\u4f7f\u7528\uff0c\u9700\u8981\u4ece Microsoft \u4e0b\u8f7d\u6587\u4ef6\u5e76\u9a8c\u8bc1\u548c\u51c6\u5907\u3002\u4ec5\u9996\u6b21\u9700\u8981 Internet \u8fde\u63a5\u3002",
    L"\u91cd\u65b0\u5b89\u88c5 Windows 7 Explorer Restorer \u5417?",
    L"\u5c06\u91cd\u65b0\u4e0b\u8f7d\u5e76\u51c6\u5907\u79c1\u6709\u7684 explorer.exe \u526f\u672c\u3002\u5982\u679c\u6b63\u5728\u4f7f\u7528\uff0c\u5c06\u5148\u5207\u6362\u56de Windows \u8d44\u6e90\u7ba1\u7406\u5668\u3002",
    L"\u5b89\u88c5",
    L"\u91cd\u65b0\u5b89\u88c5",
    L"\u4e2d\u6b62",
    L"\u4e0d\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650\u3002",
    L"\u5927\u7ea6\u9700\u8981\u4e00\u5206\u949f\u3002",
    L"\u6b63\u5728\u5b89\u88c5\u2026",
    L"\u5b89\u88c5\u5931\u8d25(\u9000\u51fa\u4ee3\u7801 %lu)\u3002\r\n\r\n%s",
    L"\u5b89\u88c5\u5931\u8d25 \u2014 \u89c1\u8be6\u60c5\u3002",
    L"\u5b89\u88c5\u5df2\u4e2d\u6b62\u3002",
    L"\u627e\u4e0d\u5230\u5b89\u88c5\u7a0b\u5e8f:\r\n%s\r\n\r\n\u8bf7\u5c06 Win7ExplorerRestorer.exe \u590d\u5236\u5230\u6b64\u5207\u6362\u5668\u65c1\u8fb9(\u540c\u4e00\u6587\u4ef6\u5939)\u5e76\u91cd\u8bd5\u3002",
    L"\u627e\u4e0d\u5230\u5b89\u88c5\u7a0b\u5e8f\u3002",
    L"\u65e0\u6cd5\u542f\u52a8\u5b89\u88c5\u7a0b\u5e8f(\u9519\u8bef %lu):\r\n%s",
    L"\u9009\u62e9\u7528\u4f5c Windows \u5916\u58f3\u7684 Explorer\u3002\u5207\u6362\u5373\u65f6\u751f\u6548\uff0c\u65e0\u9700\u6ce8\u9500\u3002",
    L"\u9ed8\u8ba4\u7cfb\u7edf\u5916\u58f3",
    L"  \u2013 \u4f7f\u7528\u4e2d",
    L"\u6bcf\u6b21\u767b\u5f55\u65f6\u4f7f\u7528 Windows 7 Explorer Restorer",
    L"\u66f4\u591a\u4fe1\u606f",
    L"\u91cd\u65b0\u5b89\u88c5",
    L"\u5378\u8f7d",
    L"\u8bed\u8a00",
    L"\u4f7f\u7528 Win7ExplorerRestorer",
    L"\u4f7f\u7528\u539f\u751f Explorer",
    L"\u684c\u9762\u5c06\u77ed\u6682\u91cd\u542f\u3002",
    L"\u767b\u5f55\u65f6\u81ea\u52a8\u542f\u52a8\u4f1a\u5c06\u6309\u7528\u6237\u7684 Shell \u503c(HKCU)\u8bbe\u7f6e\u4e3a\u79c1\u6709\u7684 explorer.exe \u2014 \u6807\u51c6\u7684 Windows "
    L"\u65b9\u5f0f\uff0c\u65e0\u9700\u63d0\u5347\uff0c\u5b8c\u5168\u53ef\u8fd8\u539f(\u4ee5\u524d\u7684\u503c\u4f1a\u88ab\u4fdd\u5b58\u5e76\u6309\u5b57\u8282\u539f\u6837\u8fd8\u539f)\u3002\r\n\r\n\u8fd8\u4f1a\u6dfb\u52a0\u4e24\u9053\u5b89\u5168\u7f51: \u542f\u52a8\u6587\u4ef6\u5939\u4e2d\u7684\u94fe\u63a5\u548c\u8ba1\u5212\u4efb\u52a1(\"7explorer "
    L"Shell Recovery\")\uff0c\u5728\u767b\u5f55\u540e\u7ea6 30 \u79d2\u68c0\u67e5\u5916\u58f3\u3002\r\n\r\n\u53d6\u6d88\u9009\u4e2d\u8be5\u590d\u9009\u6846\u53ef\u64a4\u9500\u4e00\u5207\u3002",
    L"\u91cd\u65b0\u5b89\u88c5\u5b8c\u6210\u3002\r\n\r\n\u73b0\u5728\u5207\u6362\u56de Windows 7 Explorer Restorer \u5417?",
    L"\u5c06\u5220\u9664 Windows 7 Explorer Restorer: \u4f1a\u5207\u6362\u56de Windows \u8d44\u6e90\u7ba1\u7406\u5668\uff0c\u5e76\u5220\u9664\u79c1\u6709\u6587\u4ef6\u3002\r\n\r\n\u7ee7\u7eed\u5417?",
    L"\u5df2\u5220\u9664 Windows 7 Explorer Restorer\u3002",
    L"Deutsch",
    L"Espa\u00f1ol",
    L"Fran\u00e7ais",
    L"\u65e5\u672c\u8a9e",
    L"Polski",
    L"Portugu\u00eas (BR)",
    L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439",
    L"\u4e2d\u6587(\u7b80\u4f53)",
};
static const WCHAR** TR_TABLES[UI_LANG_COUNT] = {
    TR_EN, TR_IT, TR_DE, TR_ES, TR_FR, TR_JA, TR_PL, TR_PTBR, TR_RU, TR_ZHCN
};
static_assert(ARRAYSIZE(TR_EN) == TR_COUNT, "TR_EN entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_IT) == TR_COUNT, "TR_IT entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_DE) == TR_COUNT, "TR_DE entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_ES) == TR_COUNT, "TR_ES entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_FR) == TR_COUNT, "TR_FR entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_JA) == TR_COUNT, "TR_JA entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_PL) == TR_COUNT, "TR_PL entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_PTBR) == TR_COUNT, "TR_PTBR entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_RU) == TR_COUNT, "TR_RU entries must match TR_COUNT");
static_assert(ARRAYSIZE(TR_ZHCN) == TR_COUNT, "TR_ZHCN entries must match TR_COUNT");
static UiLang g_uiLang = UI_EN;   // switcher UI language (process-local)
static const WCHAR* TR(TRID id)
{
    // Bounds-checked with English fallback: the UI always works in English.
    const WCHAR* s = nullptr;
    if ((unsigned)id < (unsigned)TR_COUNT && (unsigned)g_uiLang < (unsigned)UI_LANG_COUNT)
        s = TR_TABLES[g_uiLang][id];
    if (!s && (unsigned)id < (unsigned)TR_COUNT)
        s = TR_EN[id];
    return s ? s : L"";
}

// Maps a language code ("en", "pt-BR", "zh-CN", legacy "i"/"e", ...) to UiLang.
// Returns UI_LANG_COUNT when unknown. Never throws (pure comparisons).
static UiLang LangFromCode(const WCHAR* code)
{
    if (!code || !*code) return UI_LANG_COUNT;
    if (!lstrcmpiW(code, L"en") || !lstrcmpiW(code, L"eng")) return UI_EN;
    if (!lstrcmpiW(code, L"it") || !lstrcmpiW(code, L"ita")) return UI_IT;
    if (!lstrcmpiW(code, L"de") || !lstrcmpiW(code, L"deu")) return UI_DE;
    if (!lstrcmpiW(code, L"es") || !lstrcmpiW(code, L"esp")) return UI_ES;
    if (!lstrcmpiW(code, L"fr") || !lstrcmpiW(code, L"fra") || !lstrcmpiW(code, L"fre")) return UI_FR;
    if (!lstrcmpiW(code, L"ja") || !lstrcmpiW(code, L"jpn")) return UI_JA;
    if (!lstrcmpiW(code, L"pl") || !lstrcmpiW(code, L"pol")) return UI_PL;
    if (!lstrcmpiW(code, L"pt") || !lstrcmpiW(code, L"pt-BR") || !lstrcmpiW(code, L"ptbr") ||
        !lstrcmpiW(code, L"por") || !lstrcmpiW(code, L"pt-PT")) return UI_PTBR; // pt-PT shares pt-BR
    if (!lstrcmpiW(code, L"ru") || !lstrcmpiW(code, L"rus")) return UI_RU;
    if (!lstrcmpiW(code, L"zh") || !lstrcmpiW(code, L"zh-CN") || !lstrcmpiW(code, L"zhcn") ||
        !lstrcmpiW(code, L"chs") || !lstrcmpiW(code, L"cht") || !lstrcmpiW(code, L"zh-TW") ||
        !lstrcmpiW(code, L"zh-HK")) return UI_ZHCN; // Traditional shares Simplified
    // Legacy single letters (pre-test39 only knew it/en).
    if (code[0] == L'i' || code[0] == L'I') return UI_IT;
    if (code[0] == L'e' || code[0] == L'E') return UI_EN;
    return UI_LANG_COUNT;
}

// Primary system UI language -> switcher language (English default).
static UiLang SystemUiLang()
{
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_ITALIAN:    return UI_IT;
    case LANG_GERMAN:     return UI_DE;
    case LANG_SPANISH:    return UI_ES;
    case LANG_FRENCH:     return UI_FR;
    case LANG_JAPANESE:    return UI_JA;
    case LANG_POLISH:     return UI_PL;
    case LANG_PORTUGUESE: return UI_PTBR; // only Brazilian Portuguese translated
    case LANG_RUSSIAN:    return UI_RU;
    case LANG_CHINESE:    return UI_ZHCN; // only Simplified Chinese translated
    default:              return UI_EN;
    }
}

// Resolves the switcher UI language: WIN7EXPLORERRESTORER_LANG override, else the
// system UI language, else English. Reads only our own variable / this process:
// no Windows-wide setting is ever touched. Exception-safe (English fallback).
static UiLang ResolveUiLanguage()
{
    try {
        WCHAR l[16]; // stack buffer: RAII by scope, sized for the longest code
        DWORD n = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_LANG", l, ARRAYSIZE(l) - 1);
        if (n > 0 && n < ARRAYSIZE(l) - 1) {
            l[n] = 0;
            UiLang e = LangFromCode(l);
            if (e != UI_LANG_COUNT) return e;
        }
        return SystemUiLang();
    } catch (...) {
        return UI_EN;
    }
}

// RAII for CommandLineToArgvW (LocalFree on scope exit, never leaks).
struct ArgvGuard {
    LPWSTR* v;
    explicit ArgvGuard(LPWSTR* p) : v(p) {}
    ~ArgvGuard() { if (v) LocalFree(v); }
private:
    ArgvGuard(const ArgvGuard&);
    ArgvGuard& operator=(const ArgvGuard&);
};

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

// UI-language selection for the Win7ExplorerRestorer launch: from the GUI combo (0..10) or
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
        if (sel == 3) return L"de-DE";
        if (sel == 4) return L"es-ES";
        if (sel == 5) return L"fr-FR";
        if (sel == 6) return L"ja-JP";
        if (sel == 7) return L"pl-PL";
        if (sel == 8) return L"pt-BR";
        if (sel == 9) return L"ru-RU";
        if (sel == 10) return L"zh-CN";
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
static const WCHAR kShellLangValue[] = L"ShellUILang";  // REG_DWORD 0..10 (0=system)

static int ShellLangLoad(void) {
    try {
        RegKeyGuard k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSwitcherRegKey, 0,
                          KEY_READ, k.Put()) != ERROR_SUCCESS)
            return 0;
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (RegQueryValueExW(k.h, kShellLangValue, NULL, &type,
                             (LPBYTE)&v, &cb) != ERROR_SUCCESS ||
            type != REG_DWORD || v > 10)
            return 0;
        return (int)v;
    } catch (...) {
        SwLog(L"shell-lang: load failed, using system default");
        return 0;
    }
}

static void ShellLangSave(int sel) {
    if (sel < 0 || sel > 10) sel = 0;
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
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_DE));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_ES));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_FR));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_JA));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_PL));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_PTBR));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_RU));
        SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)TR(TR_CBO_ZHCN));
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

// Last-resort guards (test41): the window procedure and the entry point must
// never let an exception escape into the OS. The inner C++ catch(...) handles
// our own throws; the outer __except also catches access violations from
// real-world failures (bad pointers, dead COM servers). Fatal/stack
// conditions are re-raised to the OS (same rule as the wrapper's SehFilter).
// No logic is changed: every non-crashing path runs exactly as before.
static LONG WINAPI SwSehFilter(const WCHAR* where, EXCEPTION_POINTERS* info)
{
    DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374) // STATUS_HEAP_CORRUPTION
        return EXCEPTION_CONTINUE_SEARCH;
    SwLog(L"SEH 0x%08X caught in %s (continuing)", code, where ? where : L"?");
    return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK WndProcBody(HWND hwnd, UINT msg, WPARAM wParam,
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
// C++ layer (C2712/C2713 forbid mixing both EH forms in one function, so the
// C++ catch lives here and the SEH __except in the plain thunk below).
static LRESULT CALLBACK WndProcCpp(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam)
{
    try {
        return WndProcBody(hwnd, msg, wParam, lParam);
    } catch (...) {
        SwLog(L"C++ exception in WndProc msg=0x%X (continuing)", msg);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
{
    __try {
        return WndProcCpp(hwnd, msg, wParam, lParam);
    } __except (SwSehFilter(L"WndProc", GetExceptionInformation())) {
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

int WINAPI wWinMainBody(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)nCmdShow;
    g_hInst = hInstance;

    // UI language (test39): explicit WIN7EXPLORERRESTORER_LANG > system UI language
    // (English fallback). Process-local only.
    g_uiLang = ResolveUiLanguage();

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
    //   --lang=<code>      force the switcher UI language
    //                      (en it de es fr ja pl pt-BR ru zh-CN)
    {
        int argc = 0;
        ArgvGuard argv(lpCmdLine && *lpCmdLine ? CommandLineToArgvW(GetCommandLineW(), &argc)
                                             : NULL);
        int mode = 0;   // 0=GUI, 1=win7explorerestorer, 2=native, 3=install, 4=uninstall,
                        // 5=hotkey, 6=recover
        BOOL logonLink = FALSE;
        for (int i = 1; i < argc; i++) {
            if (!lstrcmpiW(argv.v[i], L"--apply-win7explorerestorer"))        mode = 1;
            else if (!lstrcmpiW(argv.v[i], L"--apply-native"))   mode = 2;
            else if (!lstrcmpiW(argv.v[i], L"--install-login"))  mode = 3;
            else if (!lstrcmpiW(argv.v[i], L"--uninstall-login"))mode = 4;
            else if (!lstrcmpiW(argv.v[i], L"--hotkey"))         mode = 5;
            else if (!lstrcmpiW(argv.v[i], L"--recover-login"))  mode = 6;
            else if (!lstrcmpiW(argv.v[i], L"--logon"))          logonLink = TRUE;
            else if (!_wcsnicmp(argv.v[i], L"--lang=", 7)) {
                // Forced switcher UI language (never touches Windows settings).
                UiLang forced = UI_LANG_COUNT;
                try { forced = LangFromCode(argv.v[i] + 7); } catch (...) { forced = UI_LANG_COUNT; }
                if (forced != UI_LANG_COUNT) g_uiLang = forced;
            }
        }

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

int WINAPI wWinMainCpp(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                       LPWSTR lpCmdLine, int nCmdShow)
{
    try {
        return wWinMainBody(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
    } catch (...) {
        SwLog(L"C++ exception in wWinMain (exiting 1)");
    }
    return 1;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    __try {
        return wWinMainCpp(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
    } __except (SwSehFilter(L"wWinMain", GetExceptionInformation())) {
    }
    return 1;
}
