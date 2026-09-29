// See NetworkIcon.h. Everything touching foreign code or external data runs
// under SEH; no C++ exceptions (the wrapper has no CRT EH support).
#include "NetworkIcon.h"
#include "SafeGuards.h"
#include "OptionConfig.h"
#include <shlwapi.h>
#include <unknwn.h>

#include "dbgprint.h"

namespace ex7 {
void LogText(const wchar_t* text);                       // ShellFixes.cpp
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def); // ShellFixes.cpp
namespace net {
namespace {

void Log(const wchar_t* fmt, ...)
{
	wchar_t msg[900], line[960];
	va_list ap; va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg), fmt, ap);
	va_end(ap);
	wnsprintfW(line, ARRAYSIZE(line), L"[ex7][net] %s", msg);
	LogText(line);
}
DWORD ReadAdvancedDword(const wchar_t* n, DWORD d) { return ReadAdvancedDwordPublic(n, d); }

// pnidui.dll 10.0.22621.3810 x64 (same build ExplorerPatcher downloads).
// SHA-256 measured on CI on the symbol server copy.
const wchar_t kDllUrl[] = L"https://msdl.microsoft.com/download/symbols/pnidui.dll/F717CABC20B000/pnidui.dll";
const char kDllSha[] = "52e9c88cb50ef98e683839ab62880180f0dd3db3e1c445ea461ae63d97a349d3";
const wchar_t kMuiUrl[] = L"https://raw.githubusercontent.com/valinet/ExplorerPatcher/0a88a6e0ef6b1752fea36e581cffff1097e862b0/ep_setup/resources/files/pnidui/%s/pnidui.dll.mui";

struct Mui { const wchar_t* lang; const char* sha; };
const Mui kMui[] = {
{ L"ar-SA", "7e96344f53bbd2c3ae126c08735e6a9cdbd34485ce4ae97edb67d9b7119e64ec" },
{ L"bg-BG", "d3604d512686c09d1633fd4a4a830f9c6f119d0b3885e1485ea568b56e14a35b" },
{ L"ca-ES", "976f131352fb71ce2ce425465afd9970d70f090769a1691a1734a2ee889c7e88" },
{ L"cs-CZ", "624fb07263e31ff74ad6fb1d7a88adc7c3119d804761b53a51e11929c17675f7" },
{ L"da-DK", "05abcca6df8762c25da5c583927b1af1ccdc913e2b3b44ae5cae6b4f37417ccf" },
{ L"de-DE", "2b3ea1f33885fd05c4dcf6b43eb58f18c918a0147f40e3a5a0c3a8c1cd02d755" },
{ L"el-GR", "b6d7e527710afb5bb5421af486b0cc0d120c9d4f5e12dc7e97b433524c9bbd5e" },
{ L"en-GB", "fc66a0ec4d664aed30effb94f2a7131166a130f713b5f29a633e0e5c504dde4a" },
{ L"en-US", "ae0d2655cb9806b5c16b92f82d698f5c98feaef1f78ebdd6014dccc8304184ed" },
{ L"es-ES", "696af95af16e8144eb07fbd2750886a68428c0d6d79f95764167e2f68eb1d494" },
{ L"es-MX", "7a8e8d5200212f32be24dc45dea129b976812cfecf9ef4f9077114d70d9fe26e" },
{ L"et-EE", "df1baa50f39378d74c398bd22f2a22a5455ed246e8d39ddda5f9018a02479a8e" },
{ L"eu-ES", "8cec7866ea0ae3264f51840c09ba03edc2abe480b9159f80d0726d57707fbf41" },
{ L"fi-FI", "bedfd57d0eb4b1619aa6524af07422179c9d7fa2e7b4ef5fff926ed601c61e72" },
{ L"fr-CA", "f160551adc9311404010838f5d9faab198084d27b4968e972a6c83409baa68ef" },
{ L"fr-FR", "dfed7530ec9f812c6b494f457113d0f5078ffe82da266848a62db442de8cfd6b" },
{ L"gl-ES", "588b5816bd3f6f29c0fa169be8741938230297fc6c013a261f11ab96b8197a9d" },
{ L"he-IL", "15a3fc6fe3515b1b9b7ff0fcbb7bf97f31a720e6dd3bc8707ff4a7b3a5c2de6b" },
{ L"hr-HR", "91a15b721395f2f8592a50c601fb3a0ff7106b96081e05b1c169006157b38f56" },
{ L"hu-HU", "8ef0129df78184408d95f2c9f41f4823261238b49be3eef5cf3d5f107dd7cd08" },
{ L"id-ID", "cb92ab0b42c7d3bd4f37dc6d10ec09e1bfe35a4f11ce790a61d2131627a93251" },
{ L"it-IT", "e7ce6e6e43483815b79946b05e6f744e9277a123ef387485826d558533609f20" },
{ L"ja-JP", "5b3abe1dc46ddd078bb484df97d10677167f0a75bfca76ba4b09d08062e99b2a" },
{ L"ko-KR", "6e9ab5d8640ddce0f6d092c0f3776b40c35e760b4437949303cd4ebf3ebca0b6" },
{ L"lt-LT", "6b51935c39c6559b262a4297ecca33995af3239934c5d20a1ddd187f447abab7" },
{ L"lv-LV", "890ef34d5ced47441631b442759094484cde6fb462248f1b83986800ff40b868" },
{ L"nb-NO", "e7aa072abdf77773650a400829553dd0fdb61fba5db87130c8f87bf170b6cfdc" },
{ L"nl-NL", "756cfd8e839e44732b3ea02ed5755107d89ebb6c733d90b2d8c1a307171f09fa" },
{ L"pl-PL", "69e5cf863bbc4848932db5d0ee7b0564164f71b42a4aa6e8e423a4a201d99776" },
{ L"pt-BR", "9cccea708617f23620e409dd543dea09bcd25a5fe016403caa131489dd0fff28" },
{ L"pt-PT", "e092ace67b909099d9332b83b38750a03115869d30f109ea8f870f49ea8731f3" },
{ L"ro-RO", "f5ab80adde536a5636231865e37cc81bb7b3ed82aae7fc1f398705c027d5e047" },
{ L"ru-RU", "2f755d4d9d19a3b6106984dbdead51e406dccb702433476d92983b81de908048" },
{ L"sk-SK", "9055867fbb6dc74b0a2e7cb5de2821f648d322afec370dd36b51eb45c836562d" },
{ L"sl-SI", "3dd32aed62437806ff406394bb32d0546f41639465a6439f566d29091fb8d0f8" },
{ L"sr-Latn-RS", "c429d15b1729a386af7e284b4c5303da3ab71bcb115b255b2d3aa8059e058791" },
{ L"sv-SE", "05c6d39553c0019d54fd5069630d765f9f48f843ffe6e71ecd03d4f5c36648dd" },
{ L"th-TH", "2818060b01c0c8e9cefde794078c7ce0472b825707131152f867b25c88795caf" },
{ L"tr-TR", "752f0bd5fc7424fe4dc93eb2a9670741d519db579b869f6c957540e1e35b52f1" },
{ L"uk-UA", "59c86cc35ac4832b4e2dfefd0e274abc902746b6151696c1345f713a1f0b92aa" },
{ L"vi-VN", "0707d9c571a496660cc2c061c2ba571b4692435d7c24b51fae9c16e6f38d1f7c" },
{ L"zh-CN", "838568b60a7ce5f700fe8a3d1e7e884576da463a087f21c8e1238723af393a51" },
{ L"zh-TW", "253aa6f2ac60197daefd4498d47c0f98129319def776ba8467be2b685b875b61" },
};

const CLSID kClsidNetworkTraySSO = { 0xC2796011, 0x81BA, 0x4148, { 0x8F, 0xCA, 0xC6, 0x64, 0x32, 0x45, 0x11, 0x3F } };
const GUID kClsidWindowsToGoSSO = { 0x4DC9C264, 0x730E, 0x4CF6, { 0x83, 0x74, 0x70, 0xF0, 0x79, 0xE4, 0xF8, 0x2B } };

wchar_t g_dir[MAX_PATH];
volatile LONG g_dllVerified = 0;   // cached dll hash checked in this process
volatile LONG g_stobjectPatched = 0;
volatile LONG g_ssoCreated = 0;
HMODULE g_pnidui = nullptr;

typedef HRESULT(WINAPI* CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstance_t g_origCoCreate = nullptr;
typedef HRESULT(WINAPI* URLDownloadToFileW_t)(LPUNKNOWN, LPCWSTR, LPCWSTR, DWORD, LPVOID);
typedef HRESULT(STDAPICALLTYPE* DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);

bool InitDir()
{
	if (g_dir[0]) return true;
	wchar_t base[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (!n || n >= MAX_PATH - 64) return false;
	wchar_t d[MAX_PATH];
	wnsprintfW(d, MAX_PATH, L"%s\\7explorer", base); CreateDirectoryW(d, nullptr);
	wnsprintfW(d, MAX_PATH, L"%s\\7explorer\\pnidui-F717CABC20B000", base); CreateDirectoryW(d, nullptr);
	if (GetFileAttributesW(d) == INVALID_FILE_ATTRIBUTES) return false;
	lstrcpynW(g_dir, d, MAX_PATH);
	return true;
}

void DllPath(wchar_t* out) { wnsprintfW(out, MAX_PATH, L"%s\\pnidui.dll", g_dir); }

bool HashIs(const wchar_t* path, const char* sha)
{
	char hex[65];
	return w81::Sha256OfFile(path, hex) && lstrcmpA(hex, sha) == 0;
}

bool SystemHasPnidui()
{
	wchar_t p[MAX_PATH];
	GetSystemDirectoryW(p, MAX_PATH); lstrcatW(p, L"\\pnidui.dll");
	return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

bool Enabled()
{
	DWORD v = ReadAdvancedDword(L"LegacyNetworkIcon", 1);
	if (v == 0) return false;
	return v == 2 || !SystemHasPnidui(); // 2 = force even when the system has one
}

// ------------------------------------------------------------ download
bool Download(const wchar_t* url, const wchar_t* dst, const char* sha)
{
	if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
		if (HashIs(dst, sha)) return true;
		Log(L"%s: hash mismatch, re-downloading", dst);
		DeleteFileW(dst);
	}
	HMODULE urlmon = LoadLibraryExW(L"urlmon.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	auto pDl = urlmon ? (URLDownloadToFileW_t)GetProcAddress(urlmon, "URLDownloadToFileW") : nullptr;
	if (!pDl) { Log(L"urlmon unavailable"); if (urlmon) FreeLibrary(urlmon); return false; }
	wchar_t part[MAX_PATH];
	wnsprintfW(part, MAX_PATH, L"%s.part", dst);
	DeleteFileW(part);
	HRESULT hr = pDl(nullptr, url, part, 0, nullptr);
	FreeLibrary(urlmon);
	if (FAILED(hr)) { Log(L"download %s failed hr=0x%08X", url, (DWORD)hr); DeleteFileW(part); return false; }
	if (!HashIs(part, sha)) { Log(L"download %s: SHA-256 mismatch, discarded", url); DeleteFileW(part); return false; }
	if (!MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		Log(L"move %s failed (%u)", dst, GetLastError()); DeleteFileW(part); return false;
	}
	Log(L"downloaded and verified %s", dst);
	return true;
}

const Mui* FindMui(const wchar_t* lang)
{
	for (const Mui& m : kMui) if (lstrcmpiW(m.lang, lang) == 0) return &m;
	return nullptr;
}

bool PrepareMui(const wchar_t* lang)
{
	const Mui* m = FindMui(lang);
	if (!m) { Log(L"no pnidui .mui for %s", lang); return false; }
	wchar_t d[MAX_PATH], dst[MAX_PATH], url[400];
	wnsprintfW(d, MAX_PATH, L"%s\\%s", g_dir, m->lang); CreateDirectoryW(d, nullptr);
	wnsprintfW(dst, MAX_PATH, L"%s\\pnidui.dll.mui", d);
	wnsprintfW(url, ARRAYSIZE(url), kMuiUrl, m->lang);
	return Download(url, dst, m->sha);
}

void PrepareUnsafe()
{
	if (!Enabled()) { Log(L"not needed (system pnidui.dll present or LegacyNetworkIcon=0)"); return; }
	if (!InitDir()) { Log(L"cache dir unavailable"); return; }
	wchar_t dll[MAX_PATH]; DllPath(dll);
	bool ok = Download(kDllUrl, dll, kDllSha);
	wchar_t langs[512]; ULONG n = 0, cch = ARRAYSIZE(langs);
	if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, langs, &cch))
		for (const wchar_t* p = langs; *p; p += lstrlenW(p) + 1) PrepareMui(p);
	PrepareMui(L"en-US");
	Log(L"cache %s: %s", g_dir, ok ? L"ready" : L"incomplete (icon from the next start after a successful download)");
}

DWORD WINAPI Worker(LPVOID)
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	SafeInvoke(L"pnidui prepare", PrepareUnsafe);
	if (SUCCEEDED(hr)) CoUninitialize();
	return 0;
}

// ------------------------------------------------------------ pnidui patches
typedef LSTATUS(WINAPI* RegGetValueW_t)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
RegGetValueW_t g_origRegGetValueW = nullptr;

// Same as ExplorerPatcher: without the 22621 registry defaults pnidui would
// look for a "ReplaceVan" value that 24H2 no longer ships; default it to 0.
LSTATUS WINAPI Pnidui_RegGetValueW(HKEY k, LPCWSTR sub, LPCWSTR val, DWORD fl, LPDWORD type, PVOID data, LPDWORD cb)
{
	LSTATUS st = g_origRegGetValueW(k, sub, val, fl, type, data, cb);
	__try {
		if (st == ERROR_FILE_NOT_FOUND && val && lstrcmpiW(val, L"ReplaceVan") == 0 && data && cb && *cb >= sizeof(DWORD)) {
			*(DWORD*)data = 0; *cb = sizeof(DWORD);
			if (type) *type = REG_DWORD;
			st = ERROR_SUCCESS;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return st;
}

void PatchPnidui(HMODULE h)
{
	HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
	g_origRegGetValueW = kb ? (RegGetValueW_t)GetProcAddress(kb, "RegGetValueW") : nullptr;
	if (g_origRegGetValueW)
		ChangeImportedAddress(h, (LPSTR)"api-ms-win-core-registry-l1-1-0.dll", g_origRegGetValueW, Pnidui_RegGetValueW);
	Log(L"pnidui %p patched (ReplaceVan default)", h);
}

// ------------------------------------------------------------ loading
struct CreateCtx { LPUNKNOWN outer; const IID* riid; void** ppv; HRESULT hr; };

void CreateUnsafe(CreateCtx* c)
{
	if (!InitDir()) return;
	wchar_t dll[MAX_PATH]; DllPath(dll);
	if (!g_pnidui) {
		if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { Log(L"pnidui not cached yet"); return; }
		if (!InterlockedCompareExchange(&g_dllVerified, 0, 0)) {
			if (!HashIs(dll, kDllSha)) { Log(L"cached pnidui failed verification: not loaded"); return; }
			InterlockedExchange(&g_dllVerified, 1);
		}
		HMODULE other = GetModuleHandleW(L"pnidui.dll");
		if (other) {
			wchar_t p[MAX_PATH] = L"";
			GetModuleFileNameW(other, p, MAX_PATH);
			if (lstrcmpiW(p, dll) != 0) { Log(L"another pnidui.dll is already loaded (%s): not loading ours", p); return; }
		}
		g_pnidui = LoadLibraryExW(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!g_pnidui) { Log(L"LoadLibraryEx(%s) failed (%u)", dll, GetLastError()); return; }
		PatchPnidui(g_pnidui);
	}
	auto pGet = (DllGetClassObject_t)GetProcAddress(g_pnidui, "DllGetClassObject");
	if (!pGet) { Log(L"pnidui without DllGetClassObject"); return; }
	IClassFactory* cf = nullptr;
	HRESULT hr = pGet(kClsidNetworkTraySSO, IID_IClassFactory, (void**)&cf);
	if (SUCCEEDED(hr) && cf) {
		hr = cf->CreateInstance(c->outer, *c->riid, c->ppv);
		cf->Release();
	}
	c->hr = hr;
	if (SUCCEEDED(hr)) InterlockedExchange(&g_ssoCreated, 1);
	Log(L"network SSO created from cache: hr=0x%08X", (DWORD)hr);
}

HRESULT WINAPI Stobject_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* ppv)
{
	bool ours = false;
	__try { ours = IsEqualCLSID(clsid, kClsidNetworkTraySSO) != FALSE; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	if (ours) {
		CreateCtx c = { outer, &riid, ppv, REGDB_E_CLASSNOTREG };
		if (!SafeInvokeCtx<CreateCtx>(L"pnidui create", CreateUnsafe, &c)) c.hr = E_FAIL;
		return c.hr;
	}
	return g_origCoCreate(clsid, outer, ctx, riid, ppv);
}

// ------------------------------------------------------------ stobject SSO table
// stobject keeps a static table of { const GUID*, int sharedThread, DWORD
// flags, BOOL (*isEnabled)() } for the SSOs it starts. The Windows To Go SSO
// is unused on consumer machines: point it at the network SSO (verified on
// CI 26100: GUID and entry found in .rdata, like ExplorerPatcher does).
struct SSOEntry { GUID* pguid; int sharedThread; DWORD flags; BOOL(*isEnabled)(); };

bool PatchSsoTableUnsafe(HMODULE st)
{
	BYTE* base = (BYTE*)st;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
	IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	BYTE* rd = nullptr; DWORD n = 0;
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		if (!memcmp(sec[i].Name, ".rdata", 6)) { rd = base + sec[i].VirtualAddress; n = sec[i].Misc.VirtualSize; break; }
	if (!rd || n < sizeof(SSOEntry)) return false;
	GUID* target = nullptr;
	for (DWORD i = 0; i + sizeof(GUID) <= n; i += 4)
		if (!memcmp(rd + i, &kClsidWindowsToGoSSO, sizeof(GUID))) { target = (GUID*)(rd + i); break; }
	if (!target) { Log(L"stobject: Windows To Go SSO GUID not found"); return false; }
	SSOEntry* e = nullptr;
	for (DWORD i = 0; i + sizeof(SSOEntry) <= n; i += 8) {
		SSOEntry* c = (SSOEntry*)(rd + i);
		if (c->pguid == target && c->sharedThread == 0 && c->flags == 0 && c->isEnabled) { e = c; break; }
	}
	if (!e) { Log(L"stobject: SSO entry not found"); return false; }
	if (e->isEnabled()) { Log(L"stobject: Windows To Go SSO is in use here, not repurposed"); return false; }
	DWORD op;
	if (!VirtualProtect(target, sizeof(GUID), PAGE_READWRITE, &op)) return false;
	*target = kClsidNetworkTraySSO;
	VirtualProtect(target, sizeof(GUID), op, &op);
	if (!VirtualProtect(e, sizeof(SSOEntry), PAGE_READWRITE, &op)) return false;
	e->sharedThread = 1; e->flags = 0; e->isEnabled = nullptr;
	VirtualProtect(e, sizeof(SSOEntry), op, &op);
	Log(L"stobject: SSO slot at +0x%X now starts the network SSO", (DWORD)((BYTE*)e - base));
	return true;
}

struct PatchCtx { HMODULE st; bool ok; };
void PatchSsoTableCtx(PatchCtx* c) { c->ok = PatchSsoTableUnsafe(c->st); }

} // namespace

void StartBackgroundPrepare()
{
	__try {
		HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool CachedDllPath(wchar_t* out)
{
	if (!InitDir()) return false;
	DllPath(out);
	return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

bool NetworkIconWanted() { return Enabled(); }
bool NetworkSsoCreated() { return g_ssoCreated != 0; }

void OnStobjectLoaded(HMODULE st)
{
	if (!st || InterlockedCompareExchange(&g_stobjectPatched, 1, 0) != 0) return;
	if (!Enabled()) return;
	if (!InitDir()) return;
	wchar_t dll[MAX_PATH]; DllPath(dll);
	if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { Log(L"pnidui not cached yet: network icon from the next start"); return; }
	PatchCtx c = { st, false };
	if (!SafeInvokeCtx<PatchCtx>(L"stobject SSO table", PatchSsoTableCtx, &c) || !c.ok) return;
	HMODULE cb = GetModuleHandleW(L"combase.dll");
	g_origCoCreate = cb ? (CoCreateInstance_t)GetProcAddress(cb, "CoCreateInstance") : nullptr;
	if (!g_origCoCreate) { Log(L"combase!CoCreateInstance not found"); return; }
	ChangeImportedAddress(st, (LPSTR)"api-ms-win-core-com-l1-1-0.dll", g_origCoCreate, Stobject_CoCreateInstance);
	ChangeImportedAddress(st, (LPSTR)"ole32.dll", g_origCoCreate, Stobject_CoCreateInstance);
	Log(L"stobject %p: CoCreateInstance routed for the network SSO", st);
}

} // namespace net
} // namespace ex7
