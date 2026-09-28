// shell_switcher.cpp — 7explorer Shell Switcher (runtime, no logout)
//
// Small native Win32 GUI tool that switches the RUNNING shell process
// between the native Windows Explorer (%SystemRoot%\explorer.exe) and the
// private 7explorer Explorer7 executable (default C:\ex7test\explorer.exe,
// overridable via the EX7_EXPLORER_PATH environment variable).
//
// What it does NOT do (by design, hard scope limits):
//   - never modifies C:\Windows\explorer.exe or any system file;
//   - never touches Winlogon, userinit, or the "Shell" registry value;
//   - never writes to the registry at all;
//   - no background service, no permanent shell replacement.
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

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600  // Vista+ (QueryFullProcessImageNameW)

// Themed standard controls (comctl32 v6); no external framework.
// (the whole /manifestdependency value must be double-quoted for the linker,
//  otherwise it splits on spaces and errors with LNK1276)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

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

#define IDC_RADIO_NATIVE  101
#define IDC_RADIO_EX7     102
#define IDC_BTN_SWITCH    201
#define IDC_BTN_CANCEL    202
#define IDC_BTN_BROWSE    203
#define IDC_CHK_LOGIN     204
#define IDC_ST_NATPATH    301
#define IDC_ST_EX7PATH    302
#define IDC_ST_CURRENT    303
#define IDC_ST_TARGET     304

#define REFRESH_TIMER_MS  1500

#define STARTUP_LINK_NAME L"7explorer-shell.lnk"

// ------------------------------------------------------------ helpers ---

static BOOL FileExists(LPCWSTR path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

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
    case SHELL_NATIVE: return L"Native Windows Explorer";
    case SHELL_EX7:    return L"Windows 7 Explorer";
    case SHELL_NONE:   return L"(none detected)";
    default:           return L"Unknown explorer (see path)";
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
        wcscpy_s(line, _countof(line), L"Current shell: (none detected)");
    } else {
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     L"Current shell: %s\r\nPID %lu \u2014 %s",
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
                 L"Target: %s\r\n%s", KindName(t),
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

// Start an executable as a detached new process. Returns FALSE+error on
// immediate launch failure.
static BOOL LaunchExe(LPCWSTR path, DWORD* pErr) {
    WCHAR cmd[1200];
    wcsncpy_s(cmd, _countof(cmd), path, _TRUNCATE);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE,
                        CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
        if (pErr) *pErr = GetLastError();
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (pErr) *pErr = 0;
    return TRUE;
}

// ---------------------------------------------------------- switching ---

// headless: TRUE when invoked from the command line (--apply-*); suppresses
// informational popups, keeps error popups (message boxes work with hwnd
// NULL). Returns 0 on success, 2 on failure (used as process exit code).
static int DoSwitch(HWND hwnd, ShellKind target, BOOL headless) {
    const WCHAR* targetPath = (target == SHELL_EX7) ? g_ex7Path
                                                    : g_nativePath;

    // 1. The target must exist BEFORE anything is stopped.
    if (!FileExists(targetPath)) {
        WCHAR msg[1300];
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     L"Target executable not found:\r\n%s\r\n\r\n"
                     L"Nothing was switched.", targetPath);
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
        _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                     L"Refusing to stop the current shell: its executable\r\n"
                     L"is not a recognized explorer:\r\n%s\r\n\r\n"
                     L"Nothing was switched.", curPath);
        MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                    MB_OK | MB_ICONERROR |
                    (headless ? MB_SYSTEMMODAL : 0));
        return 2;
    }

    if (curKind == target && pid != 0) {
        if (!headless)
            MessageBoxW(hwnd, L"The selected shell is already running.",
                        L"7explorer Shell Switcher",
                        MB_OK | MB_ICONINFORMATION);
        if (hwnd) RefreshStatus(hwnd);
        return 0;
    }

    // 3. Stop the current shell (graceful, then terminate after timeout).
    HWND shellWnd = GetShellWindow();
    if (pid != 0)
        StopShellProcess(pid, shellWnd);
    Sleep(300);  // let window classes / desks settle

    // 4. Start the target shell. On failure: restore the native shell and
    //    report; never intentionally leave the user without a shell.
    UINT mbExtra = headless ? MB_SYSTEMMODAL : 0;
    DWORD err = 0;
    int failed = 0;
    if (!LaunchExe(targetPath, &err)) {
        WCHAR msg[1500];
        failed = 1;
        if (target != SHELL_NATIVE && FileExists(g_nativePath)) {
            _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                         L"Failed to start Windows 7 Explorer:\r\n%s\r\n"
                         L"CreateProcess error %lu.\r\n\r\n"
                         L"Attempting to restore the native shell\u2026",
                         targetPath, (unsigned long)err);
            MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                        MB_OK | MB_ICONERROR | mbExtra);
            DWORD err2 = 0;
            if (!LaunchExe(g_nativePath, &err2)) {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"CRITICAL: could not start ANY shell.\r\n"
                             L"Native restore failed too (error %lu).\r\n\r\n"
                             L"Recovery: press Ctrl+Shift+Esc \u2192 Task "
                             L"Manager \u2192 Run new task \u2192 "
                             L"%s", (unsigned long)err2, g_nativePath);
                MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                            MB_OK | MB_ICONSTOP | mbExtra);
            } else {
                failed = 2;  // native restored, but the requested switch failed
            }
        } else {
            int r;
            do {
                _snwprintf_s(msg, _countof(msg), _TRUNCATE,
                             L"Failed to start the native shell:\r\n%s\r\n"
                             L"CreateProcess error %lu.\r\n\r\n"
                             L"Retry starting %s?",
                             targetPath, (unsigned long)err, targetPath);
                r = MessageBoxW(hwnd, msg, L"7explorer Shell Switcher",
                                (headless ? MB_OK : MB_RETRYCANCEL) |
                                MB_ICONERROR | mbExtra);
                if (r == IDRETRY && LaunchExe(targetPath, &err)) {
                    failed = 0;
                    break;
                }
            } while (!headless && r == IDRETRY);
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
// Optional login-time auto-switch: a link in the per-user Startup folder.
// File-based only (C:\Users\<user>\...\Startup\7explorer-shell.lnk), no
// registry, trivially removable. Requires CoInitialize by the caller path.

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
            sl->lpVtbl->SetPath(sl, own);
            sl->lpVtbl->SetArguments(sl, L"--apply-ex7");
            sl->lpVtbl->SetDescription(sl, L"7explorer shell at logon");
            IPersistFile* pf = NULL;
            hr = sl->lpVtbl->QueryInterface(sl, IID_IPersistFile,
                                            (void**)&pf);
            if (SUCCEEDED(hr)) {
                hr = pf->lpVtbl->Save(pf, linkPath, TRUE);
                pf->lpVtbl->Release(pf);
            }
            sl->lpVtbl->Release(sl);
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
    ofn.lpstrFilter = L"explorer.exe\0explorer.exe\0All files\0*.*\0";
    ofn.lpstrTitle = L"Select the private Explorer7 (explorer.exe)";
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
    MakeChild(hwnd, WC_STATICW,
              L"Switches the running Explorer shell at runtime. "
              L"No registry, no logout.",
              SS_CENTER, 10, 32, 540, 16, 0);

    MakeChild(hwnd, WC_BUTTONW, L"Select Explorer shell",
              BS_GROUPBOX, 10, 54, 540, 132, 0);

    MakeChild(hwnd, WC_BUTTONW, L"Native Windows Explorer",
              BS_AUTORADIOBUTTON | WS_TABSTOP,
              26, 76, 240, 20, IDC_RADIO_NATIVE);
    MakeChild(hwnd, WC_STATICW, g_nativePath,
              SS_LEFT, 44, 97, 490, 16, IDC_ST_NATPATH);

    MakeChild(hwnd, WC_BUTTONW, L"Windows 7 Explorer",
              BS_AUTORADIOBUTTON | WS_TABSTOP,
              26, 122, 240, 20, IDC_RADIO_EX7);
    MakeChild(hwnd, WC_STATICW, g_ex7Path,
              SS_LEFT, 44, 143, 420, 16, IDC_ST_EX7PATH);
    MakeChild(hwnd, WC_BUTTONW, L"Browse\u2026",
              BS_PUSHBUTTON | WS_TABSTOP,
              470, 140, 82, 22, IDC_BTN_BROWSE);
    MakeChild(hwnd, WC_BUTTONW,
              L"Start Windows 7 Explorer automatically at logon (user Startup folder)",
              BS_AUTOCHECKBOX | WS_TABSTOP,
              26, 176, 524, 20, IDC_CHK_LOGIN);

    MakeChild(hwnd, WC_STATICW, L"Current shell: \u2026",
              SS_LEFT, 26, 196, 524, 34, IDC_ST_CURRENT);
    MakeChild(hwnd, WC_STATICW, L"Target: \u2026",
              SS_LEFT, 26, 236, 524, 34, IDC_ST_TARGET);

    MakeChild(hwnd, WC_BUTTONW, L"Switch",
              BS_DEFPUSHBUTTON | WS_TABSTOP,
              330, 286, 100, 28, IDC_BTN_SWITCH);
    MakeChild(hwnd, WC_BUTTONW, L"Cancel",
              BS_PUSHBUTTON | WS_TABSTOP,
              440, 286, 100, 28, IDC_BTN_CANCEL);

    // Initial state: select the OTHER shell as the target, so a first
    // "Switch" actually changes something.
    WCHAR path[1024];
    DWORD pid = 0;
    ShellKind cur = DetectCurrentShell(path, (DWORD)_countof(path), &pid);
    SetRadioForKind(hwnd, (cur == SHELL_EX7) ? SHELL_NATIVE : SHELL_EX7);
    RefreshStatus(hwnd);
    UpdateTargetLabel(hwnd);
    CheckDlgButton(hwnd, IDC_CHK_LOGIN,
                   StartupIsPresent() ? BST_CHECKED : BST_UNCHECKED);

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
            if (MessageBoxW(hwnd,
                    L"Explorer will be restarted.\r\n"
                    L"Unsaved work may be affected.\r\n\r\nContinue?",
                    L"7explorer Shell Switcher",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            (void)DoSwitch(hwnd, SelectedTarget(hwnd), FALSE);
            return 0;
        }

        case IDC_BTN_BROWSE:
            if (HIWORD(wParam) == BN_CLICKED)
                BrowseForExplorer7(hwnd);
            return 0;

        case IDC_CHK_LOGIN:
            if (HIWORD(wParam) == BN_CLICKED) {
                BOOL want = (IsDlgButtonChecked(hwnd, IDC_CHK_LOGIN) ==
                             BST_CHECKED);
                DWORD e = 0;
                HRESULT hr = StartupSetPresence(want, &e);
                if (FAILED(hr)) {
                    WCHAR m[700];
                    _snwprintf_s(m, _countof(m), _TRUNCATE,
                                 L"Could not %ls the Startup-folder link "
                                 L"(error %lu).",
                                 want ? L"create" : L"remove",
                                 (unsigned long)e);
                    MessageBoxW(hwnd, m, L"7explorer Shell Switcher",
                                MB_OK | MB_ICONERROR);
                    CheckDlgButton(hwnd, IDC_CHK_LOGIN,
                                   StartupIsPresent() ? BST_CHECKED
                                                      : BST_UNCHECKED);
                } else if (want) {
                    MessageBoxW(hwnd,
                        L"A link was created in your Startup folder:\r\n"
                        L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\"
                        L"Programs\\Startup\\7explorer-shell.lnk\r\n\r\n"
                        L"At each logon this tool will switch to Explorer7 "
                        L"in the background. Uncheck the box (or delete the "
                        L"link) to remove it.",
                        L"7explorer Shell Switcher",
                        MB_OK | MB_ICONINFORMATION);
                }
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)nCmdShow;
    g_hInst = hInstance;

    ResolveShellPaths();

    // Command-line modes (used by the Startup-folder link / scripts):
    //   --apply-ex7        apply Explorer7 as shell, no confirm dialog
    //   --apply-native     apply the native shell, no confirm dialog
    //   --install-login    create the Startup-folder link
    //   --uninstall-login  remove the Startup-folder link
    {
        int argc = 0;
        LPWSTR* argv =
            lpCmdLine && *lpCmdLine ? CommandLineToArgvW(GetCommandLineW(), &argc)
                                    : NULL;
        int mode = 0;  // 0=GUI, 1=ex7, 2=native, 3=install, 4=uninstall
        for (int i = 1; i < argc; i++) {
            if (!lstrcmpiW(argv[i], L"--apply-ex7"))        mode = 1;
            else if (!lstrcmpiW(argv[i], L"--apply-native"))   mode = 2;
            else if (!lstrcmpiW(argv[i], L"--install-login"))  mode = 3;
            else if (!lstrcmpiW(argv[i], L"--uninstall-login"))mode = 4;
        }
        if (argv) LocalFree(argv);

        if (mode == 1 || mode == 2)
            return DoSwitch(NULL, mode == 1 ? SHELL_EX7 : SHELL_NATIVE, TRUE);
        if (mode == 3 || mode == 4) {
            DWORD e = 0;
            HRESULT hr = StartupSetPresence(mode == 3, &e);
            if (FAILED(hr)) {
                WCHAR m[512];
                _snwprintf_s(m, _countof(m), _TRUNCATE,
                             L"Startup-folder link operation failed "
                             L"(error %lu).", (unsigned long)e);
                MessageBoxW(NULL, m, L"7explorer Shell Switcher",
                            MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
                return 2;
            }
            return 0;
        }
    }

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
        CW_USEDEFAULT, CW_USEDEFAULT, 576, 370,
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
