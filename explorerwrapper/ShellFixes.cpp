// 7explorer fork — see ShellFixes.h.
//
// 1) Remapping of Win7 shell targets (documented CLSIDs):
//    * "Customize..." in the notification overflow launches
//        ::{26EE0668-A00A-44D7-9371-BEB064C98683}\0\::{05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9}
//      (Control Panel category view \ Notification Area Icons). Windows 10/11
//      no longer resolve that category path ("file not found"); the item
//      itself is still reachable as shell:::{05D7B0F4-...}. If that fails too
//      (newer Windows 11 builds), fall back to ms-settings:taskbar.
//    * "Connect To" uses ::{38A98528-6CBF-4CA9-8DC0-B1E1D10F7B1B} (Win7 network
//      "Connect To" pop-up), which does not exist any more. Redirect to
//      Network Connections shell:::{7007ACC7-3202-11D1-AAD2-00805FC1270E},
//      fallback ms-settings:network.
//    Every call is logged with dbgprintf so unknown targets can be captured
//    with DebugView.
// 2) "Help and Support" name: the Start menu item shows the display name of
//    CLSID {2559a1f1-21d7-11d4-bdaf-00c04f60b9f0}. When its LocalizedString
//    cannot be resolved Windows falls back to the English default value.
//    We point a per-user LocalizedString at this DLL (string 7021, localized
//    by ex7_languages.rc). Opt-out: FixHelpAndSupportName = 0.
// 3) DWM transparency: accent policies are rendered opaque when the Windows
//    "transparency effects" switch (Personalize\EnableTransparency) is 0.
//    Opt-out: KeepSystemTransparency = 1.
#include "ShellFixes.h"
#include "SafeGuards.h"
#include "OptionConfig.h"
#include <shellapi.h>
#include <commctrl.h>
#include "MinHook.h"
#include "LegacyBatteryFlyout.h"

void CreateTwinUI_UWP(); // ImmersiveShell.cpp

namespace ex7 {
namespace {

const wchar_t kNotifyIconsClsid[] = L"05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9";
const wchar_t kConnectToClsid[]   = L"38A98528-6CBF-4CA9-8DC0-B1E1D10F7B1B";
const wchar_t kHelpClsidKey[]     = L"Software\\Classes\\CLSID\\{2559a1f1-21d7-11d4-bdaf-00c04f60b9f0}";
const wchar_t kAdvancedKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
const wchar_t kPersonalizeKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

// ------------------------------------------------------------ diagnostic log
// %TEMP%\7explorer-shellfix.log (capped at 256 KB). Written in addition to
// OutputDebugString so users without DebugView can send it. Opt-out:
// ShellFixLog = 0.
bool g_logEnabled = true;
SRWLOCK g_logLock = SRWLOCK_INIT;

void LogLine(LPCWSTR fmt, ...)
{
	wchar_t msg[1024];
	va_list ap;
	va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg) - 3, fmt, ap);
	va_end(ap);
	OutputDebugStringW(msg);
	if (!g_logEnabled) return;

	wchar_t path[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, path);
	if (!n || n > MAX_PATH - 32) return;
	StringCchCatW(path, MAX_PATH, L"7explorer-shellfix.log");

	AcquireSRWLockExclusive(&g_logLock);
	ScopedHandle h(CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (h.Get() != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER size = {};
		if (GetFileSizeEx(h.Get(), &size) && size.QuadPart > 256 * 1024) {
			// full: stop logging (delete the file to start over)
			ReleaseSRWLockExclusive(&g_logLock);
			return;
		}
		SYSTEMTIME st; GetLocalTime(&st);
		char line[3200];
		wchar_t full[1100];
		wnsprintfW(full, ARRAYSIZE(full), L"%02u:%02u:%02u.%03u [%u] %s\r\n",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), msg);
		int len = WideCharToMultiByte(CP_UTF8, 0, full, -1, line, sizeof(line), nullptr, nullptr);
		if (len > 1) {
			DWORD written = 0;
			WriteFile(h.Get(), line, (DWORD)(len - 1), &written, nullptr);
		}
	}
	ReleaseSRWLockExclusive(&g_logLock);
}

typedef BOOL(WINAPI* ShellExecuteExW_t)(SHELLEXECUTEINFOW*);
typedef HINSTANCE(WINAPI* ShellExecuteW_t)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
ShellExecuteExW_t g_origShellExecuteExW = nullptr;
ShellExecuteW_t g_origShellExecuteW = nullptr;

// ------------------------------------------------------------ registry
DWORD ReadAdvancedDword(const wchar_t* name, DWORD def)
{
	DWORD v = def, cb = sizeof(v), type = 0;
	ScopedRegKey k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_READ, k.Put()) == ERROR_SUCCESS &&
		RegQueryValueExW(k.Get(), name, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
		type == REG_DWORD)
		return v;
	ScopedRegKey m;
	cb = sizeof(v);
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAdvancedKey, 0, KEY_READ, m.Put()) == ERROR_SUCCESS &&
		RegQueryValueExW(m.Get(), name, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
		type == REG_DWORD)
		return v;
	return def;
}

void WriteAdvancedDword(const wchar_t* name, DWORD v)
{
	ScopedRegKey k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, nullptr, 0, KEY_SET_VALUE,
		nullptr, k.Put(), nullptr) == ERROR_SUCCESS)
		RegSetValueExW(k.Get(), name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

// ------------------------------------------------------------ remapping
struct Remap {
	const wchar_t* explorerArgs; // passed to %windir%\explorer.exe
	const wchar_t* fallbackUri;  // ms-settings: URI
};

bool Contains(const wchar_t* s, const wchar_t* needle)
{
	return s && *s && StrStrIW(s, needle) != nullptr;
}

const Remap* FindRemap(const wchar_t* target)
{
	static const Remap notify = { L"shell:::{05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9}", L"ms-settings:taskbar" };
	static const Remap connect = { L"shell:::{7007ACC7-3202-11D1-AAD2-00805FC1270E}", L"ms-settings:network" };
	if (Contains(target, kNotifyIconsClsid)) return &notify;
	if (Contains(target, kConnectToClsid)) return &connect;
	return nullptr;
}

// Parsing name of an IDList (RAII-owned string), empty on failure.
bool IdListName(PCIDLIST_ABSOLUTE pidl, wchar_t* out, size_t cch)
{
	out[0] = 0;
	if (!pidl) return false;
	ScopedCoTaskMem name;
	if (FAILED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, name.PutStr())) || !name.Str())
		return false;
	return SUCCEEDED(StringCchCopyW(out, cch, name.Str()));
}

BOOL LaunchRemap(const Remap* r, HWND hwnd, int nShow)
{
	wchar_t explorer[MAX_PATH];
	if (!ExpandEnvironmentStringsW(L"%SystemRoot%\\explorer.exe", explorer, MAX_PATH))
		StringCchCopyW(explorer, MAX_PATH, L"explorer.exe");

	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
	sei.hwnd = hwnd;
	sei.lpFile = explorer;
	sei.lpParameters = r->explorerArgs;
	sei.nShow = nShow ? nShow : SW_SHOWNORMAL;
	if (g_origShellExecuteExW(&sei)) {
		LogLine(L"[ex7] remapped to explorer %s", r->explorerArgs);
		return TRUE;
	}
	LogLine(L"[ex7] explorer %s failed (%u), trying %s", r->explorerArgs, GetLastError(), r->fallbackUri);
	SHELLEXECUTEINFOW fb = { sizeof(fb) };
	fb.fMask = SEE_MASK_NOASYNC;
	fb.hwnd = hwnd;
	fb.lpFile = r->fallbackUri;
	fb.nShow = SW_SHOWNORMAL;
	return g_origShellExecuteExW(&fb);
}

struct ExecCtx {
	SHELLEXECUTEINFOW* sei;
	BOOL handled;
	BOOL result;
};

void TryRemapExec(ExecCtx* c)
{
	SHELLEXECUTEINFOW* sei = c->sei;
	if (!sei || sei->cbSize < sizeof(SHELLEXECUTEINFOW)) return;

	wchar_t pidlName[1024];
	const wchar_t* target = sei->lpFile;
	if ((sei->fMask & (SEE_MASK_IDLIST | SEE_MASK_INVOKEIDLIST)) && sei->lpIDList &&
		IdListName((PCIDLIST_ABSOLUTE)sei->lpIDList, pidlName, ARRAYSIZE(pidlName)))
		target = (target && *target) ? target : pidlName;

	LogLine(L"[ex7] ShellExecuteEx file=%s params=%s pidl=%s",
		sei->lpFile ? sei->lpFile : L"", sei->lpParameters ? sei->lpParameters : L"",
		(target == pidlName) ? pidlName : L"");

	const Remap* r = FindRemap(target);
	if (!r && target != pidlName && sei->lpIDList &&
		IdListName((PCIDLIST_ABSOLUTE)sei->lpIDList, pidlName, ARRAYSIZE(pidlName)))
		r = FindRemap(pidlName);
	if (!r) return;

	c->handled = TRUE;
	c->result = LaunchRemap(r, sei->hwnd, sei->nShow);
	if (c->result) {
		sei->hInstApp = (HINSTANCE)(INT_PTR)33; // > 32 means success
		if (sei->fMask & SEE_MASK_NOCLOSEPROCESS) sei->hProcess = nullptr;
	} else {
		sei->hInstApp = (HINSTANCE)(INT_PTR)SE_ERR_FNF;
	}
}

BOOL WINAPI ShellExecuteExW_Hook(SHELLEXECUTEINFOW* sei)
{
	ExecCtx c = { sei, FALSE, FALSE };
	if (!SafeInvokeCtx<ExecCtx>(L"ShellExecuteExW remap", TryRemapExec, &c))
		c.handled = FALSE; // any fault in our code: behave exactly like before
	if (c.handled) return c.result;
	return g_origShellExecuteExW(sei);
}

HINSTANCE WINAPI ShellExecuteW_Hook(HWND hwnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show)
{
	const Remap* r = nullptr;
	__try { r = FindRemap(file); if (!r) r = FindRemap(params); }
	__except (SehFilter(L"ShellExecuteW remap", GetExceptionInformation())) { r = nullptr; }
	if (r) {
		LogLine(L"[ex7] ShellExecute remap file=%s", file ? file : L"");
		return LaunchRemap(r, hwnd, show) ? (HINSTANCE)(INT_PTR)33 : (HINSTANCE)(INT_PTR)SE_ERR_FNF;
	}
	return g_origShellExecuteW(hwnd, op, file, params, dir, show);
}

void InstallExecHooks()
{
	// MinHook on the shell32 exports instead of explorer's IAT: test12 used
	// the IAT and never fired (explorer resolves these through delay-load /
	// shell32-internal paths), a function-body hook catches every caller.
	HMODULE shell32 = LoadLibraryW(L"shell32.dll");
	if (!shell32) return;
	void* exw = (void*)GetProcAddress(shell32, "ShellExecuteExW");
	void* w = (void*)GetProcAddress(shell32, "ShellExecuteW");
	MH_Initialize(); // MH_ERROR_ALREADY_INITIALIZED is fine
	if (exw) {
		MH_STATUS a = MH_CreateHook(exw, (void*)ShellExecuteExW_Hook, (void**)&g_origShellExecuteExW);
		MH_STATUS b = MH_EnableHook(exw);
		LogLine(L"[ex7] ShellExecuteExW minhook create=%d enable=%d", a, b);
		if (a != MH_OK) g_origShellExecuteExW = (ShellExecuteExW_t)exw;
	}
	if (w) {
		MH_STATUS a = MH_CreateHook(w, (void*)ShellExecuteW_Hook, (void**)&g_origShellExecuteW);
		MH_STATUS b = MH_EnableHook(w);
		LogLine(L"[ex7] ShellExecuteW minhook create=%d enable=%d", a, b);
		if (a != MH_OK) g_origShellExecuteW = (ShellExecuteW_t)w;
	}
}

// ------------------------------------------------------------ Help name
HMODULE g_self = nullptr;

void CopyAllValues(HKEY from, HKEY to)
{
	for (DWORD i = 0;; ++i) {
		wchar_t name[256];
		DWORD cchName = ARRAYSIZE(name), type = 0, cb = 0;
		LSTATUS st = RegEnumValueW(from, i, name, &cchName, nullptr, &type, nullptr, &cb);
		if (st == ERROR_NO_MORE_ITEMS) break;
		if (st != ERROR_SUCCESS || cb > 64 * 1024) continue;
		BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, cb ? cb : 1);
		if (!buf) break;
		cchName = ARRAYSIZE(name);
		if (RegEnumValueW(from, i, name, &cchName, nullptr, &type, buf, &cb) == ERROR_SUCCESS)
			RegSetValueExW(to, name, 0, type, buf, cb);
		HeapFree(GetProcessHeap(), 0, buf);
	}
}

void FixHelpAndSupportName()
{
	if (ReadAdvancedDword(L"FixHelpAndSupportName", 1) == 0) return;

	ScopedRegKey machine;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kHelpClsidKey, 0, KEY_READ, machine.Put()) != ERROR_SUCCESS) {
		LogLine(L"[ex7] Help and Support CLSID not registered, skipping");
		return;
	}
	wchar_t dll[MAX_PATH];
	DWORD n = GetModuleFileNameW(g_self, dll, MAX_PATH);
	if (!n || n >= MAX_PATH) return;
	wchar_t value[MAX_PATH + 16];
	// no printf family here: the wrapper links without the CRT stdio
	if (FAILED(StringCchCopyW(value, ARRAYSIZE(value), L"@")) ||
		FAILED(StringCchCatW(value, ARRAYSIZE(value), dll)) ||
		FAILED(StringCchCatW(value, ARRAYSIZE(value), L",-7021"))) return;

	ScopedRegKey user;
	DWORD disp = 0;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kHelpClsidKey, 0, nullptr, 0, KEY_READ | KEY_WRITE,
		nullptr, user.Put(), &disp) != ERROR_SUCCESS)
		return;
	if (disp == REG_CREATED_NEW_KEY) {
		// A per-user CLSID key shadows the machine key's values in the merged
		// HKCR view: copy them first so nothing but the name changes.
		CopyAllValues(machine.Get(), user.Get());
	} else {
		wchar_t cur[MAX_PATH + 16] = {};
		DWORD cb = sizeof(cur) - sizeof(wchar_t), type = 0;
		if (RegQueryValueExW(user.Get(), L"LocalizedString", nullptr, &type, (LPBYTE)cur, &cb) == ERROR_SUCCESS &&
			!StrStrIW(cur, L"wrp64") && !StrStrIW(cur, dll)) {
			LogLine(L"[ex7] user LocalizedString for Help already customised (%s), leaving it", cur);
			return;
		}
	}
	RegSetValueExW(user.Get(), L"LocalizedString", 0, REG_EXPAND_SZ, (const BYTE*)value,
		(DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
	LogLine(L"[ex7] Help and Support LocalizedString -> %s", value);

	// Tooltip: the machine InfoTip points at a resource id that on current
	// Windows builds resolves to an unrelated string ("changes apply to all
	// users..."). Use our own 7001 (localized in ex7_languages.rc).
	wchar_t tip[MAX_PATH + 16];
	if (SUCCEEDED(StringCchCopyW(tip, ARRAYSIZE(tip), L"@")) &&
		SUCCEEDED(StringCchCatW(tip, ARRAYSIZE(tip), dll)) &&
		SUCCEEDED(StringCchCatW(tip, ARRAYSIZE(tip), L",-7001")))
	{
		wchar_t oldTip[512] = {};
		DWORD cbTip = sizeof(oldTip) - sizeof(wchar_t), tType = 0;
		if (RegQueryValueExW(machine.Get(), L"InfoTip", nullptr, &tType, (LPBYTE)oldTip, &cbTip) == ERROR_SUCCESS)
			LogLine(L"[ex7] Help and Support machine InfoTip was %s", oldTip);
		RegSetValueExW(user.Get(), L"InfoTip", 0, REG_EXPAND_SZ, (const BYTE*)tip,
			(DWORD)((lstrlenW(tip) + 1) * sizeof(wchar_t)));
		LogLine(L"[ex7] Help and Support InfoTip -> %s", tip);
	}
}

// ------------------------------------------------------------ transparency
void EnsureTransparencyEffects()
{
	if (ReadAdvancedDword(L"KeepSystemTransparency", 0) == 1) return;
	ScopedRegKey k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_READ | KEY_SET_VALUE, k.Put()) != ERROR_SUCCESS)
		return;
	DWORD v = 1, cb = sizeof(v), type = 0;
	if (RegQueryValueExW(k.Get(), L"EnableTransparency", nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
		type == REG_DWORD && v == 0) {
		DWORD one = 1;
		RegSetValueExW(k.Get(), L"EnableTransparency", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
		LogLine(L"[ex7] EnableTransparency was 0: enabled (opt-out KeepSystemTransparency=1)");
	}
}

// ------------------------------------------------------------ battery icon
// The power icon is owned by stobject.dll (SysTray). On Windows 10/11 its
// left click asks for the modern battery flyout, which the Win7 taskbar
// cannot host, so the click does nothing. We learn which icon is the battery
// from Shell_NotifyIconW calls coming from stobject (tooltip contains the
// current battery percentage), subclass its owner window and turn a left
// click into ms-settings:batterysaver (fallback: Power Options).
// Opt-out: BatteryFlyoutFallback = 0 (original behaviour).
typedef BOOL(WINAPI* Shell_NotifyIconW_t)(DWORD, PNOTIFYICONDATAW);
Shell_NotifyIconW_t g_origNotifyIcon = nullptr;
HMODULE g_stobject = nullptr;
volatile HWND g_battHwnd = nullptr;
volatile UINT g_battId = 0;
volatile UINT g_battMsg = 0;
volatile UINT g_battVersion = 0;
volatile LONG g_battSubclassed = 0;
const UINT_PTR kBattSubclassId = 0x37E7;

bool LooksLikeBatteryTip(const wchar_t* tip)
{
	SYSTEM_POWER_STATUS ps;
	if (!tip || !*tip || !GetSystemPowerStatus(&ps)) return false;
	if (ps.BatteryFlag == 128 || ps.BatteryLifePercent > 100) return false; // no battery
	wchar_t pct[8];
	wnsprintfW(pct, ARRAYSIZE(pct), L"%u", (UINT)ps.BatteryLifePercent);
	return StrChrW(tip, L'%') != nullptr && StrStrW(tip, pct) != nullptr;
}

void LaunchBatterySettings()
{
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
	sei.lpFile = L"ms-settings:batterysaver";
	sei.nShow = SW_SHOWNORMAL;
	BOOL ok = g_origShellExecuteExW ? g_origShellExecuteExW(&sei) : ShellExecuteExW(&sei);
	if (!ok) {
		SHELLEXECUTEINFOW fb = { sizeof(fb) };
		fb.fMask = SEE_MASK_NOASYNC;
		fb.lpFile = L"control.exe";
		fb.lpParameters = L"/name Microsoft.PowerOptions";
		fb.nShow = SW_SHOWNORMAL;
		ok = g_origShellExecuteExW ? g_origShellExecuteExW(&fb) : ShellExecuteExW(&fb);
	}
	LogLine(L"[ex7] battery click -> settings launched=%d", ok);
}

LRESULT CALLBACK BatterySubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
	if (msg == g_battMsg && g_battMsg != 0) {
		UINT iconId, ev;
		if (g_battVersion >= 4) { ev = LOWORD(lParam); iconId = HIWORD(lParam); }
		else { ev = (UINT)lParam; iconId = (UINT)wParam; }
		if (iconId == g_battId) {
			if (ev == NIN_SELECT || ev == NIN_KEYSELECT || (g_battVersion < 3 && ev == WM_LBUTTONUP)) {
				LogLine(L"[ex7] battery icon event 0x%X", ev);
				LaunchBatterySettings();
				return 0;
			}
			if (ev == WM_LBUTTONDOWN || ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK)
				return 0; // swallow: the original handler only opens the dead flyout
		}
	}
	if (msg == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, BatterySubclassProc, id);
		InterlockedExchange(&g_battSubclassed, 0);
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

struct NotifyCtx { DWORD msg; PNOTIFYICONDATAW nid; };

void InspectNotify(NotifyCtx* c)
{
	PNOTIFYICONDATAW nid = c->nid;
	if (!nid || nid->cbSize < NOTIFYICONDATAW_V2_SIZE) return;
	if (c->msg == NIM_SETVERSION && nid->hWnd == g_battHwnd && nid->uID == g_battId) {
		g_battVersion = nid->uVersion;
		return;
	}
	if (c->msg != NIM_ADD && c->msg != NIM_MODIFY) return;
	if (!(nid->uFlags & NIF_TIP) || !LooksLikeBatteryTip(nid->szTip)) return;
	if (g_battHwnd != nid->hWnd || g_battId != nid->uID) {
		LogLine(L"[ex7] battery icon found hwnd=%p id=%u cb=0x%X tip=%s", nid->hWnd, nid->uID,
			(nid->uFlags & NIF_MESSAGE) ? nid->uCallbackMessage : 0, nid->szTip);
	}
	g_battHwnd = nid->hWnd;
	g_battId = nid->uID;
	if (nid->uFlags & NIF_MESSAGE) g_battMsg = nid->uCallbackMessage;
	if (g_battVersion == 0) g_battVersion = 4; // stobject on Win10+ uses version 4

	if (g_battMsg && InterlockedCompareExchange(&g_battSubclassed, 1, 0) == 0) {
		if (GetWindowThreadProcessId(nid->hWnd, nullptr) == GetCurrentThreadId() &&
			SetWindowSubclass(nid->hWnd, BatterySubclassProc, kBattSubclassId, 0))
			LogLine(L"[ex7] battery owner window subclassed");
		else {
			InterlockedExchange(&g_battSubclassed, 0);
			LogLine(L"[ex7] battery subclass failed (other thread or error %u)", GetLastError());
		}
	}
}

BOOL WINAPI Shell_NotifyIconW_Hook(DWORD msg, PNOTIFYICONDATAW nid)
{
	HMODULE caller = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)_ReturnAddress(), &caller);
	if (!ex7::w81::IsActive() && caller && caller == (g_stobject ? g_stobject : (g_stobject = GetModuleHandleW(L"stobject.dll")))) {
		NotifyCtx c = { msg, nid };
		SafeInvokeCtx<NotifyCtx>(L"Shell_NotifyIconW inspect", InspectNotify, &c);
	}
	return g_origNotifyIcon(msg, nid);
}

void InstallBatteryFix()
{
	if (ReadAdvancedDword(L"BatteryFlyoutFallback", 1) == 0) return;
	HMODULE shell32 = LoadLibraryW(L"shell32.dll");
	void* fn = shell32 ? (void*)GetProcAddress(shell32, "Shell_NotifyIconW") : nullptr;
	if (!fn) return;
	MH_Initialize();
	MH_STATUS a = MH_CreateHook(fn, (void*)Shell_NotifyIconW_Hook, (void**)&g_origNotifyIcon);
	MH_STATUS b = MH_EnableHook(fn);
	LogLine(L"[ex7] Shell_NotifyIconW minhook create=%d enable=%d", a, b);
	if (a != MH_OK) g_origNotifyIcon = (Shell_NotifyIconW_t)fn;
}

} // namespace

// ------------------------------------------------------------ public
void InstallShellFixes(HMODULE hSelf)
{
	g_self = hSelf;
	g_logEnabled = ReadAdvancedDword(L"ShellFixLog", 1) != 0;
	LogLine(L"[ex7] ---- 7explorer shell fixes, pid %u ----", GetCurrentProcessId());
	SafeInvoke(L"InstallExecHooks", InstallExecHooks);
	SafeInvoke(L"w81 flyout prepare", ex7::w81::StartBackgroundPrepare); // real 8.1 flyout (cache/download)
	SafeInvoke(L"InstallBatteryFix", InstallBatteryFix);                 // fallback while 8.1 is unavailable
	SafeInvoke(L"FixHelpAndSupportName", FixHelpAndSupportName);
	SafeInvoke(L"EnsureTransparencyEffects", EnsureTransparencyEffects);
}

// Sentinel: ImmersiveInitFailures counts start-ups that began UWP init but
// never reported success (crash/hang). After 2 in a row UWP stays off until
// the value is deleted or reset to 0.
bool ImmersiveStartupAllowed()
{
	DWORD failures = ReadAdvancedDword(L"ImmersiveInitFailures", 0);
	if (failures >= 2) {
		LogLine(L"[ex7] UWP disabled for this session: %u failed start-ups "
			L"(reset HKCU\\...\\Explorer\\Advanced\\ImmersiveInitFailures to 0)", failures);
		return false;
	}
	return true;
}

void ImmersiveStartupBegin()
{
	WriteAdvancedDword(L"ImmersiveInitFailures", ReadAdvancedDword(L"ImmersiveInitFailures", 0) + 1);
}

void ImmersiveStartupSucceeded()
{
	WriteAdvancedDword(L"ImmersiveInitFailures", 0);
}

static void CreateTwinUIThunk() { CreateTwinUI_UWP(); }

void SafeCreateTwinUI_UWP()
{
	static LONG s_done = 0;
	if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return; // once per process
	ImmersiveStartupBegin(); // raised now, cleared only if start-up returns
	if (SafeInvoke(L"CreateTwinUI_UWP", CreateTwinUIThunk))
		ImmersiveStartupSucceeded();
	else
		LogLine(L"[ex7] TwinUI start-up faulted; failure counter left raised");
}

} // namespace ex7
