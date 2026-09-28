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

void CreateTwinUI_UWP(); // ImmersiveShell.cpp

namespace ex7 {
namespace {

const wchar_t kNotifyIconsClsid[] = L"05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9";
const wchar_t kConnectToClsid[]   = L"38A98528-6CBF-4CA9-8DC0-B1E1D10F7B1B";
const wchar_t kHelpClsidKey[]     = L"Software\\Classes\\CLSID\\{2559a1f1-21d7-11d4-bdaf-00c04f60b9f0}";
const wchar_t kAdvancedKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
const wchar_t kPersonalizeKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

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
		dbgprintf(L"[ex7] remapped to explorer %s", r->explorerArgs);
		return TRUE;
	}
	dbgprintf(L"[ex7] explorer %s failed (%u), trying %s", r->explorerArgs, GetLastError(), r->fallbackUri);
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

	dbgprintf(L"[ex7] ShellExecuteEx file=%s params=%s pidl=%s",
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
		dbgprintf(L"[ex7] ShellExecute remap file=%s", file ? file : L"");
		return LaunchRemap(r, hwnd, show) ? (HINSTANCE)(INT_PTR)33 : (HINSTANCE)(INT_PTR)SE_ERR_FNF;
	}
	return g_origShellExecuteW(hwnd, op, file, params, dir, show);
}

void InstallExecHooks()
{
	HMODULE self = GetModuleHandleW(nullptr);
	HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
	if (!shell32) return;
	g_origShellExecuteExW = (ShellExecuteExW_t)GetProcAddress(shell32, "ShellExecuteExW");
	g_origShellExecuteW = (ShellExecuteW_t)GetProcAddress(shell32, "ShellExecuteW");
	if (g_origShellExecuteExW)
		dbgprintf(L"[ex7] ShellExecuteExW hook: %d",
			ChangeImportedAddress(self, "shell32.dll", g_origShellExecuteExW, ShellExecuteExW_Hook));
	if (g_origShellExecuteW)
		dbgprintf(L"[ex7] ShellExecuteW hook: %d",
			ChangeImportedAddress(self, "shell32.dll", g_origShellExecuteW, ShellExecuteW_Hook));
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
		dbgprintf(L"[ex7] Help and Support CLSID not registered, skipping");
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
			dbgprintf(L"[ex7] user LocalizedString for Help already customised (%s), leaving it", cur);
			return;
		}
	}
	RegSetValueExW(user.Get(), L"LocalizedString", 0, REG_EXPAND_SZ, (const BYTE*)value,
		(DWORD)((lstrlenW(value) + 1) * sizeof(wchar_t)));
	dbgprintf(L"[ex7] Help and Support LocalizedString -> %s", value);
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
		dbgprintf(L"[ex7] EnableTransparency was 0: enabled (opt-out KeepSystemTransparency=1)");
	}
}

} // namespace

// ------------------------------------------------------------ public
void InstallShellFixes(HMODULE hSelf)
{
	g_self = hSelf;
	SafeInvoke(L"InstallExecHooks", InstallExecHooks);
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
		dbgprintf(L"[ex7] UWP disabled for this session: %u failed start-ups "
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
		dbgprintf(L"[ex7] TwinUI start-up faulted; failure counter left raised");
}

} // namespace ex7
