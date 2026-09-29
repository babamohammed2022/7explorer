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
#include <netlistmgr.h>
#include <ocidl.h>

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

// ---------------------------------------------------------------------------
// test30: state from Network List Manager (learn.microsoft.com, "Network List
// Manager"): every *connected* INetworkConnection gives its adapter id,
// connectivity and INetwork name. The adapter id is matched with
// GetAdaptersAddresses (AdapterName = "{GUID}") to know whether it is Wi-Fi
// (IF_TYPE_IEEE80211) or wired, and the connection that carries the default
// route (GetBestInterface) is the one shown - an Ethernet adapter that merely
// exists is never preferred over the Wi-Fi link actually in use.
// Why: on Windows 11 24H2 WlanQueryInterface(wlan_intf_opcode_current_
// connection) returns ERROR_ACCESS_DENIED unless the user granted location
// access to desktop apps ("Changes to API behavior for Wi-Fi access and
// location"); test29 used it as the only Wi-Fi test, so Wi-Fi fell through to
// the wired branch (Ethernet icon). wlan_intf_opcode_rssi is not in that list
// and is used only for the signal bars.
struct AdapterInfo { GUID id; IFTYPE type; IF_INDEX index; };
const int kMaxAdapters = 32;

typedef DWORD(WINAPI* GetBestInterface_t)(DWORD, PDWORD);

int ReadAdapters(AdapterInfo* out, int max, DWORD* bestIf)
{
	*bestIf = 0;
	ScopedLib lib(LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!lib.get()) return 0;
	auto pGet = (GetAdaptersAddresses_t)GetProcAddress(lib.get(), "GetAdaptersAddresses");
	auto pBest = (GetBestInterface_t)GetProcAddress(lib.get(), "GetBestInterface");
	if (pBest) { DWORD idx = 0; if (pBest(0x01010101 /*1.1.1.1, route lookup only*/, &idx) == NO_ERROR) *bestIf = idx; }
	if (!pGet) return 0;
	ULONG cb = 16 * 1024;
	for (int attempt = 0; attempt < 3; ++attempt) {
		HeapBuffer buf(cb);
		if (!buf.Get()) return 0;
		PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf.Get();
		ULONG r = pGet(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &cb);
		if (r == ERROR_BUFFER_OVERFLOW) continue;
		if (r != ERROR_SUCCESS) return 0;
		int n = 0;
		for (; a && n < max; a = a->Next) {
			wchar_t w[64];
			if (!a->AdapterName || !MultiByteToWideChar(CP_ACP, 0, a->AdapterName, -1, w, 64)) continue;
			if (FAILED(CLSIDFromString(w, &out[n].id))) continue;
			out[n].type = a->IfType; out[n].index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
			++n;
		}
		return n;
	}
	return 0;
}

typedef void(WINAPI* SysFreeString_t)(BSTR);
void FreeBstr(BSTR b)
{
	static SysFreeString_t p = (SysFreeString_t)GetProcAddress(LoadLibraryW(L"oleaut32.dll"), "SysFreeString");
	if (b && p) p(b);
}

// Signal bars (0-4) of a Wi-Fi adapter; -1 = unknown.
int WifiBars(const GUID& adapter)
{
	ScopedLib lib(LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!lib.get()) return -1;
	auto pOpen = (WlanOpenHandle_t)GetProcAddress(lib.get(), "WlanOpenHandle");
	auto pClose = (WlanCloseHandle_t)GetProcAddress(lib.get(), "WlanCloseHandle");
	auto pQuery = (WlanQueryInterface_t)GetProcAddress(lib.get(), "WlanQueryInterface");
	auto pFree = (WlanFreeMemory_t)GetProcAddress(lib.get(), "WlanFreeMemory");
	if (!pOpen || !pClose || !pQuery || !pFree) return -1;
	HANDLE h = nullptr; DWORD ver = 0;
	if (pOpen(2, nullptr, &ver, &h) != ERROR_SUCCESS) return -1;
	int bars = -1;
	PVOID data = nullptr; DWORD cb = 0;
	DWORD r = pQuery(h, &adapter, wlan_intf_opcode_rssi, nullptr, &cb, &data, nullptr);
	if (r == ERROR_SUCCESS && data && cb >= sizeof(LONG)) {
		LONG rssi = *(LONG*)data;
		bars = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
	}
	if (data) pFree(data);
	if (bars < 0) { // older drivers: signal quality (needs location consent on 24H2)
		PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; cb = 0;
		DWORD r2 = pQuery(h, &adapter, wlan_intf_opcode_current_connection, nullptr, &cb, (PVOID*)&ca, nullptr);
		if (r2 == ERROR_SUCCESS && ca) {
			int q = (int)ca->wlanAssociationAttributes.wlanSignalQuality;
			bars = q >= 80 ? 4 : q >= 60 ? 3 : q >= 40 ? 2 : q >= 20 ? 1 : 0;
		}
		if (ca) pFree(ca);
		static bool logged = false;
		if (!logged) { logged = true; Log(L"wlan rssi=%u current_connection=%u (5 = location access denied)", r, r2); }
	}
	pClose(h, nullptr);
	return bars;
}

struct ConnCtx { Snapshot* s; bool ok; };

void TakeNlmUnsafe(ConnCtx* c)
{
	Snapshot& s = *c->s;
	AdapterInfo ad[kMaxAdapters]; DWORD best = 0;
	int nAd = ReadAdapters(ad, kMaxAdapters, &best);
	ComPtr<INetworkListManager> nlm;
	if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, __uuidof(INetworkListManager), nlm.PutVoid())) || !nlm) return;
	ComPtr<IEnumNetworkConnections> en;
	if (FAILED(nlm->GetNetworkConnections(en.Put())) || !en) return;
	c->ok = true;
	int bestScore = -1;
	for (;;) {
		ComPtr<INetworkConnection> conn; ULONG got = 0;
		if (en->Next(1, conn.Put(), &got) != S_OK || !conn) break;
		VARIANT_BOOL connected = VARIANT_FALSE;
		if (FAILED(conn->get_IsConnected(&connected)) || connected != VARIANT_TRUE) continue;
		NLM_CONNECTIVITY cn = NLM_CONNECTIVITY_DISCONNECTED; conn->GetConnectivity(&cn);
		GUID id = {}; conn->GetAdapterId(&id);
		IFTYPE type = 0; IF_INDEX idx = 0;
		for (int i = 0; i < nAd; ++i) if (IsEqualGUID(ad[i].id, id)) { type = ad[i].type; idx = ad[i].index; break; }
		if (type == IF_TYPE_SOFTWARE_LOOPBACK || type == IF_TYPE_TUNNEL) continue;
		bool internet = (cn & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
		int score = (idx && idx == best ? 4 : 0) + (internet ? 2 : 0) + 1;
		if (score <= bestScore) continue;
		bestScore = score;
		bool wifi = type == IF_TYPE_IEEE80211;
		s.st = wifi ? (internet ? StWifiOk : StWifiLimited) : (internet ? StWiredOk : StWiredLimited);
		s.name[0] = 0;
		ComPtr<INetwork> netw;
		if (SUCCEEDED(conn->GetNetwork(netw.Put())) && netw) {
			BSTR name = nullptr;
			if (SUCCEEDED(netw->GetName(&name)) && name) { lstrcpynW(s.name, name, ARRAYSIZE(s.name)); FreeBstr(name); }
		}
		s.bars = 4;
		if (wifi) { int b = WifiBars(id); if (b >= 0) s.bars = b; }
	}
}

Snapshot Take()
{
	Snapshot s = {}; s.st = StNone;
	ConnCtx c = { &s, false };
	if (!SafeInvokeCtx<ConnCtx>(L"net icon NLM", TakeNlmUnsafe, &c)) { c.ok = false; s = Snapshot(); s.st = StNone; }
	if (c.ok) return s;
	// NLM unavailable (service stopped): previous heuristics.
	DWORD cv = Connectivity();
	bool internet = (cv & (NLM_V4_INTERNET | NLM_V6_INTERNET)) != 0;
	int q = 0;
	if (QueryWifi(s.name, ARRAYSIZE(s.name), &q)) {
		s.st = internet ? StWifiOk : StWifiLimited;
		s.bars = q >= 80 ? 4 : q >= 60 ? 3 : q >= 40 ? 2 : q >= 20 ? 1 : 0;
	} else if (WiredUp(s.name, ARRAYSIZE(s.name))) {
		s.st = internet ? StWiredOk : StWiredLimited;
	}
	return s;
}

// ---------------------------------------------------------------------------
// Change notifications (INetworkListManagerEvents / INetworkConnectionEvents /
// INetworkEvents via IConnectionPointContainer): each event only schedules a
// refresh on the icon thread (debounced), the slow timer stays as a net.
const UINT WM_NETCHANGED = WM_APP + 0x38;
HWND g_notifyWnd = nullptr;

class NetEvents : public INetworkListManagerEvents, public INetworkConnectionEvents, public INetworkEvents {
public:
	STDMETHODIMP QueryInterface(REFIID r, void** p) override {
		if (!p) return E_POINTER;
		if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, __uuidof(INetworkListManagerEvents))) *p = static_cast<INetworkListManagerEvents*>(this);
		else if (IsEqualIID(r, __uuidof(INetworkConnectionEvents))) *p = static_cast<INetworkConnectionEvents*>(this);
		else if (IsEqualIID(r, __uuidof(INetworkEvents))) *p = static_cast<INetworkEvents*>(this);
		else { *p = nullptr; return E_NOINTERFACE; }
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static lifetime
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP ConnectivityChanged(NLM_CONNECTIVITY) override { return Ping(); }
	STDMETHODIMP NetworkConnectionConnectivityChanged(GUID, NLM_CONNECTIVITY) override { return Ping(); }
	STDMETHODIMP NetworkConnectionPropertyChanged(GUID, NLM_CONNECTION_PROPERTY_CHANGE) override { return Ping(); }
	STDMETHODIMP NetworkAdded(GUID) override { return Ping(); }
	STDMETHODIMP NetworkDeleted(GUID) override { return Ping(); }
	STDMETHODIMP NetworkConnectivityChanged(GUID, NLM_CONNECTIVITY) override { return Ping(); }
	STDMETHODIMP NetworkPropertyChanged(GUID, NLM_NETWORK_PROPERTY_CHANGE) override { return Ping(); }
private:
	HRESULT Ping() { if (g_notifyWnd) PostMessageW(g_notifyWnd, WM_NETCHANGED, 0, 0); return S_OK; }
};
NetEvents g_events;
INetworkListManager* g_evNlm = nullptr; // kept for the thread lifetime

void AdviseUnsafe()
{
	if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, __uuidof(INetworkListManager), (void**)&g_evNlm)) || !g_evNlm) {
		g_evNlm = nullptr; Log(L"NLM events: CoCreateInstance failed"); return;
	}
	ComPtr<IConnectionPointContainer> cpc;
	if (FAILED(g_evNlm->QueryInterface(__uuidof(IConnectionPointContainer), cpc.PutVoid())) || !cpc) return;
	const IID* iids[] = { &__uuidof(INetworkListManagerEvents), &__uuidof(INetworkConnectionEvents), &__uuidof(INetworkEvents) };
	for (const IID* iid : iids) {
		ComPtr<IConnectionPoint> cp; DWORD cookie = 0;
		HRESULT hr = cpc->FindConnectionPoint(*iid, cp.Put());
		if (SUCCEEDED(hr) && cp) hr = cp->Advise(static_cast<INetworkListManagerEvents*>(&g_events), &cookie);
		Log(L"NLM events advise hr=0x%08X", (DWORD)hr);
	}
}

// ------------------------------------------------------------ window
const UINT WM_TRAYCB = WM_APP + 0x37;
const UINT_PTR kTimer = 1, kTimerDebounce = 2;
HMODULE g_res = nullptr;          // cached pnidui.dll as image resource
HWND g_wnd = nullptr;
UINT g_taskbarCreated = 0;
bool g_added = false;
Snapshot g_last = { (State)-1 };
HICON g_icon = nullptr;
// Win7 system tray icon GUIDs: clock 7820AE72, volume 7820AE73, network 7820AE74, power 7820AE75.
const GUID kNetworkIconGuid = { 0x7820AE74, 0x23E3, 0x4229, { 0x82, 0xC1, 0xE4, 0x1C, 0xB6, 0x7D, 0x5B, 0x9C } };
bool g_useGuid = true; // read in IconThread (no dynamic initialisers without CRT)

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
	// test35: registered with the Win7 system "Network" icon GUID, so the
	// Win7 tray treats it as the system icon (always shown, like pnidui was)
	// instead of a new app icon that auto-hide sends to the overflow area.
	// Falls back to the plain id if the tray refuses the GUID. Opt-out
	// NetworkIconSystemGuid=0.
	if (g_useGuid) { nid.uFlags |= NIF_GUID; nid.guidItem = kNetworkIconGuid; }
	nid.uCallbackMessage = WM_TRAYCB;
	HICON ico = (HICON)LoadImageW(g_res, MAKEINTRESOURCEW(IconFor(s)), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
	nid.hIcon = ico;
	Tooltip(s, nid.szTip, ARRAYSIZE(nid.szTip));
	BOOL ok;
	if (!g_added || force) {
		Shell_NotifyIconW(NIM_DELETE, &nid);
		ok = Shell_NotifyIconW(NIM_ADD, &nid);
		if (!ok && g_useGuid) {
			Log(L"NIM_ADD with the system network GUID refused (%u): plain icon", GetLastError());
			g_useGuid = false;
			nid.uFlags &= ~NIF_GUID; nid.guidItem = GUID();
			Shell_NotifyIconW(NIM_DELETE, &nid);
			ok = Shell_NotifyIconW(NIM_ADD, &nid);
		}
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

DWORD Build()
{
	typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
	OSVERSIONINFOW v = { sizeof(v) };
	auto p = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	return (p && p(&v) == 0) ? v.dwBuildNumber : 0;
}

// Reached only when no flyout mod swallowed the click.
void OnLeftClick()
{
	if (Build() < 22000) { // Windows 10: the VAN flyout still exists
		SHELLEXECUTEINFOW sei = { sizeof(sei) };
		sei.fMask = SEE_MASK_FLAG_NO_UI; sei.lpFile = L"ms-availablenetworks:"; sei.nShow = SW_SHOWNORMAL;
		if (ShellExecuteExW(&sei)) return;
	}
	Launch(L"control.exe", L"/name Microsoft.NetworkAndSharingCenter");
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
		case WM_NETCHANGED: // debounce bursts of NLM events
			SetTimer(h, kTimerDebounce, 700, nullptr);
			return 0;
		case WM_POWERBROADCAST:
			if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) SetTimer(h, kTimerDebounce, 3000, nullptr);
			return TRUE;
		case WM_TIMER:
			if (w == kTimerDebounce) { KillTimer(h, kTimerDebounce); UpdateIcon(false); return 0; }
			if (w == kTimer) {
				if (!OwnIconEngine() && NetworkSsoCreated()) { // the real icon started after all
					NOTIFYICONDATAW nid = { sizeof(nid) }; nid.hWnd = h; nid.uID = 1;
					if (g_useGuid) { nid.uFlags = NIF_GUID; nid.guidItem = kNetworkIconGuid; }
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
	g_useGuid = ReadAdvancedDwordPublic(L"NetworkIconSystemGuid", 1) != 0;
	const bool own = OwnIconEngine();
	Sleep(own ? 2000 : 15000); // SSO mode: give the stobject-hosted pnidui time to start
	if (!own && NetworkSsoCreated()) { Log(L"pnidui running: no fallback needed"); return 0; }
	wchar_t dll[MAX_PATH];
	if (!CachedDllPath(dll)) { Log(L"pnidui not cached: no icons for the fallback yet"); return 0; }
	// Loaded as a real module (DllMain only; no SSO object is created): the
	// Win7 network flyout mod (win7-network-flyout-recreation) recognises the
	// network icon by its callback window class "ATL:<address inside
	// pnidui.dll>", so our window uses such a class name and the mod's own
	// flyout opens on click (the mod is the source of truth for the UI).
	g_res = GetModuleHandleW(L"pnidui.dll");
	if (!g_res) g_res = LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_res) g_res = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!g_res) { Log(L"cannot map %s (%u)", dll, GetLastError()); return 0; }
	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	WNDCLASSW wc = {};
	wchar_t cls[40] = L"Ex7NetworkTrayIcon";
	if (((ULONG_PTR)g_res & 3) == 0) // real module (datafile handles have low bits set)
		wnsprintfW(cls, ARRAYSIZE(cls), L"ATL:%p", (void*)((BYTE*)g_res + 0x1000));
	wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
	Log(L"callback window class %s", cls);
	RegisterClassW(&wc);
	g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
	if (g_wnd) {
		g_notifyWnd = g_wnd;
		SafeInvoke(L"net icon NLM advise", AdviseUnsafe);
		SafeInvoke(L"net icon add", []() { UpdateIcon(true); });
		SetTimer(g_wnd, kTimer, 10000, nullptr); // safety net; events drive updates
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
	if (!NetworkIconWanted() && !OwnIconEngine() && ReadAdvancedDwordPublic(L"LegacyNetworkIcon", 1) != 3) return;
	__try {
		HANDLE t = CreateThread(nullptr, 0, IconThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace net
} // namespace ex7
