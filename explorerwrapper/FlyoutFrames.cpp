// See FlyoutFrames.h (credits: aubymori, Aero Flyout Fix).
#include "FlyoutFrames.h"
#include "SafeGuards.h"
#include "MinHook.h"
#include <commctrl.h>

namespace ex7 {
void LogText(const wchar_t* text);
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def);
namespace {

void Log(const wchar_t* fmt, ...)
{
	wchar_t msg[600], line[660];
	va_list ap; va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg), fmt, ap);
	va_end(ap);
	wnsprintfW(line, ARRAYSIZE(line), L"[ex7][flyout] %s", msg);
	LogText(line);
}

typedef HWND(WINAPI* CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND(WINAPI* CreateWindowInBand_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, __int64, __int64, __int64, __int64);
typedef BOOL(WINAPI* SetWindowSubclass_t)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
typedef LRESULT(WINAPI* DefSubclassProc_t)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL(WINAPI* RemoveWindowSubclass_t)(HWND, SUBCLASSPROC, UINT_PTR);

CreateWindowExW_t g_origCWEx = nullptr;
SetWindowSubclass_t g_setSubclass = nullptr;
DefSubclassProc_t g_defSubclass = nullptr;
RemoveWindowSubclass_t g_removeSubclass = nullptr;
const UINT_PTR kSubclassId = 0x37464C59; // '7FLY'

bool IsFlyoutClass(LPCWSTR cls)
{
	__try {
		if (!cls || !((ULONG_PTR)cls & ~(ULONG_PTR)0xFFFF)) return false; // atom
		return lstrcmpW(cls, L"ClockFlyoutWindow") == 0 ||
			lstrcmpW(cls, L"BatMeterFlyout") == 0 ||
			lstrcmpW(cls, L"WHCFlyoutWindow") == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

LRESULT CALLBACK FlyoutSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
{
	__try {
		switch (m) {
		case WM_NCHITTEST: {
			LRESULT r = g_defSubclass(h, m, w, l);
			switch (r) {
			case HTTOP: case HTTOPRIGHT: case HTRIGHT: case HTBOTTOMRIGHT:
			case HTBOTTOM: case HTBOTTOMLEFT: case HTLEFT: case HTTOPLEFT:
				return HTBORDER;
			}
			return r;
		}
		case WM_STYLECHANGING:
			if (w == GWL_STYLE && l) ((STYLESTRUCT*)l)->styleNew |= WS_THICKFRAME;
			else if (w == GWL_EXSTYLE && l) ((STYLESTRUCT*)l)->styleNew |= WS_EX_TOOLWINDOW;
			break;
		case WM_NCDESTROY:
			if (g_removeSubclass) g_removeSubclass(h, FlyoutSubclass, id);
			break;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return g_defSubclass(h, m, w, l);
}

void ApplyFrame(HWND h, LPCWSTR cls)
{
	if (!h) return;
	__try {
		SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_THICKFRAME);
		SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
		if (g_setSubclass) g_setSubclass(h, FlyoutSubclass, kSubclassId, 0);
		SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		Log(L"thick frame applied to %s %p", cls, h);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

HWND WINAPI CreateWindowExW_Hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int hgt,
	HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
	HWND h = g_origCWEx(ex, cls, name, style, x, y, w, hgt, parent, menu, inst, param);
	if (h && IsFlyoutClass(cls)) ApplyFrame(h, cls);
	return h;
}

// CreateWindowInBand is already MinHooked by the immersive code paths, so the
// band-created flyouts (8.1 BatMeterFlyout) are caught when they are shown.
bool HasFrame(HWND h)
{
	DWORD_PTR ref = 0;
	typedef BOOL(WINAPI* GetWindowSubclass_t)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR*);
	static GetWindowSubclass_t get = (GetWindowSubclass_t)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "GetWindowSubclass");
	if (get && get(h, FlyoutSubclass, kSubclassId, &ref)) return true;
	return !get && (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME);
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD)
{
	__try {
		if (obj != OBJID_WINDOW || !h) return;
		wchar_t cls[64];
		if (!GetClassNameW(h, cls, ARRAYSIZE(cls)) || !IsFlyoutClass(cls)) return;
		// in-context hook: we run on the thread that owns the window, so the
		// subclass can be installed here
		if (GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId() && !HasFrame(h))
			ApplyFrame(h, cls);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

DWORD WINAPI EventThread(LPVOID)
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OnShow, &self);
	HWINEVENTHOOK hk = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, self, OnShow,
		GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
	Log(L"show hook %p", hk);
	if (!hk) return 0;
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
	UnhookWinEvent(hk);
	return 0;
}

void InstallUnsafe()
{
	if (ReadAdvancedDwordPublic(L"AeroFlyoutFrames", 1) == 0) { Log(L"AeroFlyoutFrames=0"); return; }
	HMODULE cc = LoadLibraryW(L"comctl32.dll");
	if (cc) {
		g_setSubclass = (SetWindowSubclass_t)GetProcAddress(cc, "SetWindowSubclass");
		g_defSubclass = (DefSubclassProc_t)GetProcAddress(cc, "DefSubclassProc");
		g_removeSubclass = (RemoveWindowSubclass_t)GetProcAddress(cc, "RemoveWindowSubclass");
	}
	if (!g_setSubclass || !g_defSubclass) { g_setSubclass = nullptr; Log(L"comctl32 subclass API missing: frames only"); }
	MH_Initialize();
	HMODULE u = GetModuleHandleW(L"user32.dll");
	void* p1 = u ? (void*)GetProcAddress(u, "CreateWindowExW") : nullptr;
	if (p1 && MH_CreateHook(p1, (void*)CreateWindowExW_Hook, (void**)&g_origCWEx) == MH_OK) MH_EnableHook(p1);
	HANDLE t = CreateThread(nullptr, 0, EventThread, nullptr, 0, nullptr);
	if (t) CloseHandle(t);
	Log(L"hooks: CreateWindowExW=%d, show-event thread=%d", g_origCWEx != nullptr, t != nullptr);
}

} // namespace

void InstallFlyoutFrames() { SafeInvoke(L"InstallFlyoutFrames", InstallUnsafe); }

} // namespace ex7
