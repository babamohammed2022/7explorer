#include "TrayMenus.h"
#include "SafeGuards.h"
#include "NotifyIconsDialog.h"
#include "MinHook.h"
#include <intrin.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

namespace ex7 {
void LogText(const wchar_t* text);
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def);
namespace {

enum Owner { kOther = 0, kVolume, kNetwork, kStobject };

Owner OwnerOf(void* addr)
{
	HMODULE m = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)addr, &m) || !m) return kOther;
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(m, path, MAX_PATH)) return kOther;
	const wchar_t* f = PathFindFileNameW(path);
	if (!lstrcmpiW(f, L"SndVolSSO.dll")) return kVolume;
	if (!lstrcmpiW(f, L"pnidui.dll")) return kNetwork;
	if (!lstrcmpiW(f, L"stobject.dll")) return kStobject;
	return kOther;
}

typedef BOOL(WINAPI* SetMII_t)(HMENU, UINT, BOOL, LPCMENUITEMINFOW);
typedef BOOL(WINAPI* TPMEx_t)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
typedef BOOL(WINAPI* TPM_t)(HMENU, UINT, int, int, int, HWND, const RECT*);
SetMII_t g_origSet = nullptr, g_origInsert = nullptr;
TPMEx_t g_origTPMEx = nullptr;
TPM_t g_origTPM = nullptr;
bool g_strip = true, g_actions = true;

// Copy the item info without MFT_OWNERDRAW (Anixx's technique).
BOOL StripAndCall(SetMII_t fn, HMENU h, UINT item, BOOL byPos, LPCMENUITEMINFOW mii)
{
	__try {
		if (mii && (mii->fMask & MIIM_FTYPE) && (mii->fType & MFT_OWNERDRAW) && mii->cbSize <= sizeof(MENUITEMINFOW)) {
			MENUITEMINFOW copy = {};
			memcpy(&copy, mii, mii->cbSize);
			copy.fType &= ~MFT_OWNERDRAW;
			return fn(h, item, byPos, &copy);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return fn(h, item, byPos, mii);
}

BOOL WINAPI SetMII_Hook(HMENU h, UINT item, BOOL byPos, LPCMENUITEMINFOW mii)
{
	if (g_strip && OwnerOf(_ReturnAddress()) != kOther) return StripAndCall(g_origSet, h, item, byPos, mii);
	return g_origSet(h, item, byPos, mii);
}
BOOL WINAPI InsertMII_Hook(HMENU h, UINT item, BOOL byPos, LPCMENUITEMINFOW mii)
{
	if (g_strip && OwnerOf(_ReturnAddress()) != kOther) return StripAndCall(g_origInsert, h, item, byPos, mii);
	return g_origInsert(h, item, byPos, mii);
}

// Remove owner-draw from every item right before showing (covers items the
// immersive helper changed via ModifyMenu or internal paths).
void StripWholeMenu(HMENU h)
{
	__try {
		int n = GetMenuItemCount(h);
		for (int i = 0; i < n; ++i) {
			MENUITEMINFOW mi = { sizeof(mi) }; mi.fMask = MIIM_FTYPE | MIIM_SUBMENU;
			if (!GetMenuItemInfoW(h, i, TRUE, &mi)) continue;
			if (mi.fType & MFT_OWNERDRAW) {
				MENUITEMINFOW s = { sizeof(s) }; s.fMask = MIIM_FTYPE; s.fType = mi.fType & ~MFT_OWNERDRAW;
				g_origSet(h, i, TRUE, &s);
			}
			if (mi.hSubMenu) StripWholeMenu(mi.hSubMenu);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Has(const wchar_t* s, const wchar_t* n) { return StrStrIW(s, n) != nullptr; }

bool RunSys(const wchar_t* file, const wchar_t* params)
{
	wchar_t sys[MAX_PATH], exe[MAX_PATH];
	GetSystemDirectoryW(sys, MAX_PATH);
	wnsprintfW(exe, MAX_PATH, L"%s\\%s", sys, file);
	HINSTANCE r = ShellExecuteW(nullptr, nullptr, exe, params, nullptr, SW_SHOWNORMAL);
	wchar_t l[400]; wnsprintfW(l, 400, L"[ex7][menus] volume action -> %s %s: %d", exe, params ? params : L"", (int)(INT_PTR)r);
	LogText(l);
	return (INT_PTR)r > 32;
}

// true when the command was handled here.
bool VolumeAction(HMENU h, UINT cmd)
{
	wchar_t text[256] = L"";
	__try {
		if (!GetMenuStringW(h, cmd, text, 256, MF_BYCOMMAND)) return false;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	wchar_t l[400]; wnsprintfW(l, 400, L"[ex7][menus] volume menu cmd=%u text=%s", cmd, text); LogText(l);
	if (Has(text, L"mixer") || Has(text, L"mezclador") || Has(text, L"m\u00E9langeur") || Has(text, L"Lautst\u00E4rkemix"))
		return RunSys(L"SndVol.exe", nullptr);
	bool settings = Has(text, L"impostazioni") || Has(text, L"settings") || Has(text, L"einstellungen") ||
		Has(text, L"configuraci\u00F3n") || Has(text, L"param\u00E8tres");
	bool sound = Has(text, L"audio") || Has(text, L"sound") || Has(text, L"suono") || Has(text, L"sonido") || Has(text, L"son");
	if (settings && sound && !Has(text, L"spazial") && !Has(text, L"spatial"))
		return RunSys(L"control.exe", L"mmsys.cpl");
	return false;
}

DWORD g_inTrack = 0;

// Shared body: returns the value to give back to the caller.
BOOL Track(HMENU h, UINT flags, int x, int y, HWND hwnd, LPTPMPARAMS p, Owner o)
{
	if (g_strip) StripWholeMenu(h);
	if (!(g_actions && o == kVolume) || (flags & TPM_NONOTIFY && !(flags & TPM_RETURNCMD)))
		return g_origTPMEx(h, flags, x, y, hwnd, p);
	UINT cmd = (UINT)g_origTPMEx(h, flags | TPM_RETURNCMD | TPM_NONOTIFY, x, y, hwnd, p);
	if (!cmd) return 0;
	if (VolumeAction(h, cmd)) return (flags & TPM_RETURNCMD) ? 0 : TRUE;
	if (flags & TPM_RETURNCMD) return (BOOL)cmd;
	SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0); // what TrackPopupMenu would have done
	return TRUE;
}

BOOL WINAPI TPMEx_Hook(HMENU h, UINT flags, int x, int y, HWND hwnd, LPTPMPARAMS p)
{
	Owner o = OwnerOf(_ReturnAddress());
	DWORD tid = GetCurrentThreadId();
	if (o == kOther || g_inTrack == tid) return g_origTPMEx(h, flags, x, y, hwnd, p);
	g_inTrack = tid;
	BOOL r = FALSE;
	__try { r = Track(h, flags, x, y, hwnd, p, o); }
	__except (SehFilter(L"TrackPopupMenuEx hook", GetExceptionInformation())) { r = FALSE; }
	g_inTrack = 0;
	return r;
}

BOOL WINAPI TPM_Hook(HMENU h, UINT flags, int x, int y, int res, HWND hwnd, const RECT* rc)
{
	Owner o = OwnerOf(_ReturnAddress());
	DWORD tid = GetCurrentThreadId();
	if (o == kOther || g_inTrack == tid) return g_origTPM(h, flags, x, y, res, hwnd, rc);
	g_inTrack = tid;
	BOOL r = FALSE;
	__try { r = Track(h, flags, x, y, hwnd, nullptr, o); }
	__except (SehFilter(L"TrackPopupMenu hook", GetExceptionInformation())) { r = FALSE; }
	g_inTrack = 0;
	return r;
}

bool Hook(void* target, void* detour, void** orig, const wchar_t* name)
{
	if (!target) return false;
	MH_STATUS a = MH_CreateHook(target, detour, orig);
	MH_STATUS b = a == MH_OK ? MH_EnableHook(target) : a;
	wchar_t l[160]; wnsprintfW(l, 160, L"[ex7][menus] hook %s %d/%d", name, a, b); LogText(l);
	return b == MH_OK;
}

void InstallUnsafe()
{
	g_strip = ReadAdvancedDwordPublic(L"ClassicTrayMenus", 1) != 0;
	g_actions = ReadAdvancedDwordPublic(L"VolumeMenuActions", 1) != 0;
	if (!g_strip && !g_actions) return;
	MH_Initialize();
	HMODULE u = GetModuleHandleW(L"user32.dll");
	Hook((void*)GetProcAddress(u, "SetMenuItemInfoW"), (void*)SetMII_Hook, (void**)&g_origSet, L"SetMenuItemInfoW");
	Hook((void*)GetProcAddress(u, "InsertMenuItemW"), (void*)InsertMII_Hook, (void**)&g_origInsert, L"InsertMenuItemW");
	Hook((void*)GetProcAddress(u, "TrackPopupMenuEx"), (void*)TPMEx_Hook, (void**)&g_origTPMEx, L"TrackPopupMenuEx");
	Hook((void*)GetProcAddress(u, "TrackPopupMenu"), (void*)TPM_Hook, (void**)&g_origTPM, L"TrackPopupMenu");
}

// ---- IOpenControlPanel::Open ------------------------------------------------
// The Win7 explorer opens "Customize notification icons" (tray menu, overflow
// link, taskbar properties button) with IOpenControlPanel::Open(
// L"Microsoft.NotificationAreaIcons", ...). On 24H2 that item no longer
// exists, Open fails silently and nothing appears: redirect it to the
// built-in dialog.
typedef HRESULT(STDMETHODCALLTYPE* Open_t)(IOpenControlPanel*, LPCWSTR, LPCWSTR, IUnknown*);
Open_t g_origOpen = nullptr;

HRESULT STDMETHODCALLTYPE Open_Hook(IOpenControlPanel* self, LPCWSTR name, LPCWSTR page, IUnknown* site)
{
	bool mine = false;
	__try {
		mine = name && (StrStrIW(name, L"NotificationAreaIcons") || StrStrIW(name, L"05D7B0F4-2121-4EFF-BF6B-ED3F69B894D9"));
		wchar_t l[300]; wnsprintfW(l, 300, L"[ex7] IOpenControlPanel::Open name=%s page=%s", name ? name : L"", page ? page : L"");
		LogText(l);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { mine = false; }
	// The system page is used when it exists (it does on 24H2/25H2); the
	// built-in window only if Open fails or NotifyIconsUseSettings=3.
	DWORD mode = mine ? ReadAdvancedDwordPublic(L"NotifyIconsUseSettings", 0) : 0;
	if (mine && mode == 3 && ShowNotifyIconsDialog()) return S_OK;
	HRESULT hr = g_origOpen(self, name, page, site);
	if (mine) {
		wchar_t l[120]; wnsprintfW(l, 120, L"[ex7] IOpenControlPanel::Open hr=0x%08X", (DWORD)hr); LogText(l);
		if (FAILED(hr) && mode == 0 && ShowNotifyIconsDialog()) return S_OK;
	}
	return hr;
}

void InstallOpenUnsafe()
{
	const CLSID clsid = { 0x06622D85, 0x6856, 0x4460, { 0x8D, 0xE1, 0xA8, 0x19, 0x21, 0xB4, 0x1C, 0x4B } };
	const IID iid = { 0xD11AD862, 0x66DE, 0x4DF4, { 0xBF, 0x6C, 0x1F, 0x56, 0x21, 0x99, 0x6A, 0xF1 } };
	HRESULT hi = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	IOpenControlPanel* p = nullptr;
	HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void**)&p);
	if (SUCCEEDED(hr) && p) {
		void* fn = (*(void***)p)[3];
		MH_Initialize();
		Hook(fn, (void*)Open_Hook, (void**)&g_origOpen, L"IOpenControlPanel::Open");
		p->Release();
	} else {
		wchar_t l[120]; wnsprintfW(l, 120, L"[ex7] OpenControlPanel CoCreate hr=0x%08X", (DWORD)hr); LogText(l);
	}
	if (SUCCEEDED(hi)) CoUninitialize();
}

DWORD WINAPI OpenThread(LPVOID) { SafeInvoke(L"IOpenControlPanel hook", InstallOpenUnsafe); return 0; }

} // namespace

void InstallTrayMenus() { SafeInvoke(L"InstallTrayMenus", InstallUnsafe); }

void InstallControlPanelOpenHook()
{
	// own thread: COM init must not disturb the caller's apartment
	HANDLE t = CreateThread(nullptr, 0, OpenThread, nullptr, 0, nullptr);
	if (t) CloseHandle(t); // never wait: may run under the loader lock
}

} // namespace ex7
