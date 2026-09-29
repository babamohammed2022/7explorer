// 7explorer fork: fallback network notification icon (see NetworkIcon.h).
// Everything runs on one dedicated STA thread with its own hidden window.
// SEH around every callback; COM/handles owned by small RAII helpers.
#include "NetworkIcon.h"
#include "SafeGuards.h"
#include <shellapi.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <wlanapi.h>

namespace ex7 {
void LogText(const wchar_t* text);
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def);
namespace net {
namespace {

void Log(const wchar_t* fmt, ...)
{
	wchar_t msg[600], line[660];
	va_list ap; va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg), fmt, ap);
	va_end(ap);
	wnsprintfW(line, ARRAYSIZE(line), L"[ex7][net-icon] %s", msg);
	LogText(line);
}

// Network List Manager (netlistmgr.h), GUIDs defined locally.
const CLSID kClsidNLM = { 0xDCB00C01, 0x570F, 0x4A9B, { 0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B } };
const IID kIidNLM = { 0xDCB00000, 0x570F, 0x4A9B, { 0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B } };
// Only GetConnectivity (vtable slot 7 + IDispatch = index 13) is used: declare the
// interface prefix exactly as in netlistmgr.h.
struct INLM : public IDispatch {
	virtual HRESULT STDMETHODCALLTYPE GetNetworks(int, void**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetNetwork(GUID, void**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetNetworkConnections(void**) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetNetworkConnection(GUID, void**) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_IsConnectedToInternet(VARIANT_BOOL*) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_IsConnected(VARIANT_BOOL*) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetConnectivity(DWORD*) = 0;
};
const DWORD NLM_V4_INTERNET = 0x40, NLM_V6_INTERNET = 0x400, NLM_V4_ANY = 0x10 | 0x20 | 0x40, NLM_V6_ANY = 0x100 | 0x200 | 0x400;

// Icon resource ids in pnidui.dll 10.0.22621.3810 (checked on CI).
enum : WORD {
	IcoWiredOk = 3048, IcoWiredLimited = 3051, IcoDisconnected = 3020,
	IcoWifi0 = 3021, IcoWifiLimited0 = 3027, IcoWifiNone = 3067, IcoAirplane = 3300,
};

enum State { StNone, StWiredOk, StWiredLimited, StWifiOk, StWifiLimited };

struct Snapshot {
	State st; int bars; wchar_t name[128];
};

class ScopedLib {
public:
	explicit ScopedLib(HMODULE h) : m(h) {}
	~ScopedLib() { if (m) FreeLibrary(m); }
	HMODULE get() const { return m; }
private:
	HMODULE m;
	ScopedLib(const ScopedLib&) = delete; ScopedLib& operator=(const ScopedLib&) = delete;
};

class HeapBuffer {
public:
	explicit HeapBuffer(SIZE_T cb) : m_p(cb ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb) : nullptr) {}
	~HeapBuffer() { if (m_p) HeapFree(GetProcessHeap(), 0, m_p); }
	void* Get() const { return m_p; }
private:
	void* m_p;
	HeapBuffer(const HeapBuffer&) = delete; HeapBuffer& operator=(const HeapBuffer&) = delete;
};

bool Italian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN; }
const wchar_t* T(const wchar_t* it, const wchar_t* en) { return Italian() ? it : en; }

typedef DWORD(WINAPI* WlanOpenHandle_t)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD(WINAPI* WlanCloseHandle_t)(HANDLE, PVOID);
typedef DWORD(WINAPI* WlanEnumInterfaces_t)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef DWORD(WINAPI* WlanQueryInterface_t)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef VOID(WINAPI* WlanFreeMemory_t)(PVOID);

// Connected Wi-Fi: profile name + signal quality (0-100). false = no Wi-Fi link.
bool QueryWifi(wchar_t* name, size_t cch, int* quality)
{
	ScopedLib lib(LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!lib.get()) return false;
	auto pOpen = (WlanOpenHandle_t)GetProcAddress(lib.get(), "WlanOpenHandle");
	auto pClose = (WlanCloseHandle_t)GetProcAddress(lib.get(), "WlanCloseHandle");
	auto pEnum = (WlanEnumInterfaces_t)GetProcAddress(lib.get(), "WlanEnumInterfaces");
	auto pQuery = (WlanQueryInterface_t)GetProcAddress(lib.get(), "WlanQueryInterface");
	auto pFree = (WlanFreeMemory_t)GetProcAddress(lib.get(), "WlanFreeMemory");
	if (!pOpen || !pClose || !pEnum || !pQuery || !pFree) return false;
	HANDLE h = nullptr; DWORD ver = 0;
	if (pOpen(2, nullptr, &ver, &h) != ERROR_SUCCESS) return false;
	bool found = false;
	PWLAN_INTERFACE_INFO_LIST list = nullptr;
	if (pEnum(h, nullptr, &list) == ERROR_SUCCESS && list) {
		for (DWORD i = 0; i < list->dwNumberOfItems && !found; ++i) {
			if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
			PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; DWORD cb = 0;
			if (pQuery(h, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection,
				nullptr, &cb, (PVOID*)&ca, nullptr) == ERROR_SUCCESS && ca) {
				lstrcpynW(name, ca->strProfileName, (int)cch);
				*quality = (int)ca->wlanAssociationAttributes.wlanSignalQuality;
				found = true;
				pFree(ca);
			}
		}
		pFree(list);
	}
	pClose(h, nullptr);
	return found;
}

typedef ULONG(WINAPI* GetAdaptersAddresses_t)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);

// Any wired (non Wi-Fi, non loopback/tunnel) adapter up with a gateway?
bool WiredUp(wchar_t* name, size_t cch)
{
	ScopedLib lib(LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	auto pGet = lib.get() ? (GetAdaptersAddresses_t)GetProcAddress(lib.get(), "GetAdaptersAddresses") : nullptr;
	if (!pGet) return false;
	ULONG cb = 16 * 1024;
	for (int attempt = 0; attempt < 3; ++attempt) {
		HeapBuffer buf(cb);
		if (!buf.Get()) return false;
		PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf.Get();
		ULONG r = pGet(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, a, &cb);
		if (r == ERROR_BUFFER_OVERFLOW) continue;
		if (r != ERROR_SUCCESS) return false;
		for (; a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
			if (a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
			lstrcpynW(name, a->FriendlyName ? a->FriendlyName : L"", (int)cch);
			return true;
		}
		return false;
	}
	return false;
}

DWORD Connectivity()
{
	INLM* nlm = nullptr;
	DWORD c = 0;
	if (SUCCEEDED(CoCreateInstance(kClsidNLM, nullptr, CLSCTX_ALL, kIidNLM, (void**)&nlm)) && nlm) {
		if (FAILED(nlm->GetConnectivity(&c))) c = 0;
		nlm->Release();
	}
	return c;
}

Snapshot Take()
{
	Snapshot s = {}; s.st = StNone;
	DWORD c = Connectivity();
	bool internet = (c & (NLM_V4_INTERNET | NLM_V6_INTERNET)) != 0;
	bool any = (c & (NLM_V4_ANY | NLM_V6_ANY)) != 0;
	int q = 0;
	if (QueryWifi(s.name, ARRAYSIZE(s.name), &q)) {
		s.st = internet ? StWifiOk : StWifiLimited;
		s.bars = q >= 80 ? 4 : q >= 60 ? 3 : q >= 40 ? 2 : q >= 20 ? 1 : 0;
	} else if (WiredUp(s.name, ARRAYSIZE(s.name)) || any) {
		s.st = internet ? StWiredOk : StWiredLimited;
	}
	return s;
}

// ------------------------------------------------------------ window
const UINT WM_TRAYCB = WM_APP + 0x37;
const UINT_PTR kTimer = 1;
HMODULE g_res = nullptr;          // cached pnidui.dll as image resource
HWND g_wnd = nullptr;
UINT g_taskbarCreated = 0;
bool g_added = false;
Snapshot g_last = { (State)-1 };
HICON g_icon = nullptr;

WORD IconFor(const Snapshot& s)
{
	switch (s.st) {
	case StWiredOk: return IcoWiredOk;
	case StWiredLimited: return IcoWiredLimited;
	case StWifiOk: return (WORD)(IcoWifi0 + s.bars);
	case StWifiLimited: return (WORD)(IcoWifiLimited0 + (s.bars > 4 ? 4 : s.bars));
	default: return IcoDisconnected;
	}
}

void Tooltip(const Snapshot& s, wchar_t* out, size_t cch)
{
	const wchar_t* acc =
		(s.st == StWiredOk || s.st == StWifiOk) ? T(L"Accesso a Internet", L"Internet access") :
		(s.st == StNone) ? T(L"Non connesso - Connessioni disponibili", L"Not connected - Connections are available") :
		T(L"Nessun accesso a Internet", L"No Internet access");
	if (s.st == StNone || !s.name[0]) lstrcpynW(out, acc, (int)cch);
	else wnsprintfW(out, (int)cch, L"%s\n%s", s.name, acc);
}

void UpdateIcon(bool force)
{
	Snapshot s = Take();
	bool same = !force && s.st == g_last.st && s.bars == g_last.bars && lstrcmpW(s.name, g_last.name) == 0;
	if (same && g_added) return;
	NOTIFYICONDATAW nid = { sizeof(nid) };
	nid.hWnd = g_wnd; nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
	nid.uCallbackMessage = WM_TRAYCB;
	HICON ico = (HICON)LoadImageW(g_res, MAKEINTRESOURCEW(IconFor(s)), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	nid.hIcon = ico;
	Tooltip(s, nid.szTip, ARRAYSIZE(nid.szTip));
	BOOL ok;
	if (!g_added || force) {
		Shell_NotifyIconW(NIM_DELETE, &nid);
		ok = Shell_NotifyIconW(NIM_ADD, &nid);
		nid.uVersion = NOTIFYICON_VERSION_4;
		if (ok) Shell_NotifyIconW(NIM_SETVERSION, &nid);
		g_added = ok != FALSE;
	} else ok = Shell_NotifyIconW(NIM_MODIFY, &nid);
	if (g_icon) DestroyIcon(g_icon);
	g_icon = ico;
	if (!same) Log(L"state %d bars %d name '%s' icon %u -> %d", s.st, s.bars, s.name, IconFor(s), ok);
	g_last = s;
}

void Launch(const wchar_t* file, const wchar_t* params)
{
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_FLAG_NO_UI;
	sei.lpFile = file; sei.lpParameters = params; sei.nShow = SW_SHOWNORMAL;
	BOOL ok = ShellExecuteExW(&sei);
	Log(L"launch %s %s -> %d (%u)", file, params ? params : L"", ok, ok ? 0 : GetLastError());
}

void OnLeftClick()
{
	// Win10/11 network list flyout; Settings page as fallback.
	SHELLEXECUTEINFOW sei = { sizeof(sei) };
	sei.fMask = SEE_MASK_FLAG_NO_UI; sei.lpFile = L"ms-availablenetworks:"; sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&sei)) Launch(L"ms-settings:network", nullptr);
}

void OnMenu(HWND h)
{
	HMENU m = CreatePopupMenu();
	if (!m) return;
	AppendMenuW(m, MF_STRING, 1, T(L"Risoluzione problemi", L"Troubleshoot problems"));
	AppendMenuW(m, MF_STRING, 2, T(L"Apri Centro connessioni di rete e condivisione", L"Open Network and Sharing Center"));
	AppendMenuW(m, MF_STRING, 3, T(L"Impostazioni di rete e Internet", L"Network and Internet settings"));
	SetMenuDefaultItem(m, 2, FALSE);
	POINT pt; GetCursorPos(&pt);
	SetForegroundWindow(h);
	UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
	DestroyMenu(m);
	PostMessageW(h, WM_NULL, 0, 0);
	switch (cmd) {
	case 1: Launch(L"msdt.exe", L"-id NetworkDiagnosticsNetworkAdapter"); break;
	case 2: Launch(L"control.exe", L"/name Microsoft.NetworkAndSharingCenter"); break;
	case 3: Launch(L"ms-settings:network", nullptr); break;
	}
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	__try {
		if (msg == g_taskbarCreated && g_taskbarCreated) { g_added = false; UpdateIcon(true); return 0; }
		switch (msg) {
		case WM_TIMER:
			if (w == kTimer) {
				if (GetModuleHandleW(L"pnidui.dll")) { // the real icon started after all
					NOTIFYICONDATAW nid = { sizeof(nid) }; nid.hWnd = h; nid.uID = 1;
					Shell_NotifyIconW(NIM_DELETE, &nid); g_added = false;
					KillTimer(h, kTimer); Log(L"pnidui is running: fallback icon removed");
					return 0;
				}
				UpdateIcon(false);
			}
			return 0;
		case WM_TRAYCB:
			switch (LOWORD(l)) {
			case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: OnLeftClick(); break;
			case WM_CONTEXTMENU: case WM_RBUTTONUP: OnMenu(h); break;
			}
			return 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return DefWindowProcW(h, msg, w, l);
}

DWORD WINAPI IconThread(LPVOID)
{
	Sleep(15000); // give the stobject-hosted pnidui time to start
	if (GetModuleHandleW(L"pnidui.dll")) { Log(L"pnidui running: no fallback needed"); return 0; }
	wchar_t dll[MAX_PATH];
	if (!CachedDllPath(dll)) { Log(L"pnidui not cached: no icons for the fallback yet"); return 0; }
	g_res = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!g_res) { Log(L"cannot map %s (%u)", dll, GetLastError()); return 0; }
	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Ex7NetworkTrayIcon";
	RegisterClassW(&wc);
	g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
	if (g_wnd) {
		SafeInvoke(L"net icon add", []() { UpdateIcon(true); });
		SetTimer(g_wnd, kTimer, 4000, nullptr);
		MSG m;
		while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
	}
	if (SUCCEEDED(hrCo)) CoUninitialize();
	return 0;
}

volatile LONG g_started = 0;

} // namespace

void StartFallbackTrayIcon()
{
	if (InterlockedExchange(&g_started, 1)) return;
	if (!NetworkIconWanted() && ReadAdvancedDwordPublic(L"LegacyNetworkIcon", 1) != 3) return;
	__try {
		HANDLE t = CreateThread(nullptr, 0, IconThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace net
} // namespace ex7
