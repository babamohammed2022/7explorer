// 7explorer fork — Windows 8.1 battery flyout as a downloaded dependency.
//
// Findings (CI analysis of the real 6.3.9600.17415 binaries, see
// tools/analyze_flyout.py, tools/w81_compat_check.py, tools/w81_mui_probe.py):
//  * stobject.dll exports only DllGetClassObject/DllCanUnloadNow and is the
//    SysTray shell service object CLSID {35CEC8A3-2BE6-11D2-8773-92E220524153}
//    (the object explorer.exe creates with CoCreateInstance). The battery
//    flyout itself lives in stobject.dll: DirectUI (DUI70.dll) UI, resource
//    UIFILE 500, element names "BatMeterFlyout", "FlyoutElement".
//  * batmeter.dll is not just resources: it is the battery data engine
//    (CreateBatteryData, GetBatteryStatusText, ...), statically imported by
//    stobject as "BatMeter.dll". Its exports match the current system one by
//    name and ordinal, so the 8.1 copy must be loaded FIRST, by full path,
//    or stobject would bind to the system copy.
//  * Neither file carries an embedded Authenticode signature (Windows system
//    files are catalog-signed and the 8.1 catalogs do not exist on 10/11):
//    WinVerifyTrust returns TRUST_E_NOSIGNATURE. Integrity therefore rests on
//    the pinned SHA-256 (exact bytes) + PE TimeDateStamp/SizeOfImage check.
//  * All strings live in the language .mui. The 8.1 .mui is not available,
//    and the loader rejects the current system .mui (checksum mismatch). The
//    current system .mui re-keyed with the 8.1 checksums IS accepted (53
//    strings for stobject, 26 for batmeter on 24H2 en-US): we generate that
//    file locally from the user's own system file (nothing redistributed).
//  * On Windows 11 24H2 (26100) every import resolves except the delay-load
//    SETTINGSYNCPOLICY.dll (absent). Delay loads go through the OS
//    DelayLoadFailureHook, which returns failure stubs instead of raising.
//  * DllGetClassObject(CLSID_SysTray) + CreateInstance(IOleCommandTarget)
//    succeed on 26100 outside explorer.
//
// Runtime design:
//  1. StartBackgroundPrepare (DllMain) -> worker thread (never the shell
//     thread): if the machine has a battery and the build is supported and
//     the cache is incomplete: download each file with URLDownloadToFileW to
//     "<name>.part" in the cache dir, verify SHA-256 + PE header, move to the
//     final name only after verification. One attempt per 24h after a
//     failure (W81FlyoutLastFailure), no loops. Then build re-keyed .mui.
//  2. TryCreateSysTray (shell thread, CoCreateInstance hook): cache only.
//     Re-hash both files, refuse if the system batmeter.dll is already
//     loaded, LoadLibraryEx batmeter then stobject (full path), IAT-patch
//     CreateWindowInBand -> CreateWindowExW fallback, DllGetClassObject ->
//     IClassFactory::CreateInstance. Any failure -> system stobject.
//  3. Everything that touches foreign code or external data runs under SEH.
//     C++ try/catch is NOT used: the wrapper is built without C++ exceptions
//     and without the CRT (no vcruntime EH personality); nothing in this file
//     throws C++ exceptions (no STL, no throwing new).
//
// Configuration: DWORDs/strings in HKCU\Software\Microsoft\Windows\
//   CurrentVersion\Explorer\Advanced
//   LegacyBatteryFlyout  DWORD 1 (default) / 0 = off
//   W81FlyoutForce       DWORD 1 = ignore the build table
//   W81StobjectId / W81StobjectSha256 / W81BatmeterId / W81BatmeterSha256
//                        REG_SZ: alternative 8.1 build (id = TimeDateStamp
//                        %08X + SizeOfImage %x, as in the symbol server URL)
#include "LegacyBatteryFlyout.h"
#include "SafeGuards.h"
#include "OSVersion.h"
#include <shellapi.h>

namespace ex7 {
namespace w81 {
namespace {

// ------------------------------------------------------------ tables
struct Package {
	const wchar_t* name;
	const wchar_t* id;      // symbol server key: TimeDateStamp(%08X) + SizeOfImage(%x)
	DWORD timeDateStamp;
	DWORD sizeOfImage;
	const char* sha256;     // lowercase hex
};

// 6.3.9600.17415 (winblue_r4.141028-1500), verified on CI: HTTP 200, hash match.
const Package kDefault[2] = {
	{ L"batmeter.dll", L"545054931f3000", 0x54505493, 0x1F3000,
	  "f32f18d44f9a6511c73ca1a9a4a6edad38aff23a15fd4c75d9aaaaf31526a506" },
	{ L"stobject.dll", L"54503A4356000", 0x54503A43, 0x56000,
	  "30737741f7131ff80706c7d12b2fe8ab8a6203aeaf9984d9a7c908c0f2565149" },
};

struct BuildRange { DWORD minBuild, maxBuild; bool supported; const wchar_t* note; };
// Same idea as the offset tables: explicit ranges, anything else = off.
// Only 26100 has been load-tested (CI); the rest is "expected to work".
const BuildRange kBuilds[] = {
	{ 10240, 19045, true,  L"Windows 10 1507-22H2" },
	{ 19046, 21999, false, L"Insider/unknown" },
	{ 22000, 26200, true,  L"Windows 11 21H2-25H2" },
	{ 26201, 0xFFFFFFFF, false, L"newer than tested" },
};

const wchar_t kAdvancedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
const wchar_t kSysTrayClsidStr[] = L"{35CEC8A3-2BE6-11D2-8773-92E220524153}";
const CLSID kClsidSysTray = { 0x35CEC8A3, 0x2BE6, 0x11D2, { 0x87, 0x73, 0x92, 0xE2, 0x20, 0x52, 0x41, 0x53 } };

// ------------------------------------------------------------ state
struct PackageCfg {
	wchar_t name[32];
	wchar_t id[32];
	DWORD tds, soi;
	char sha[65];
};
PackageCfg g_pkg[2];
wchar_t g_cacheDir[MAX_PATH];
volatile LONG g_ready = 0;      // cache verified by the worker
volatile LONG g_active = 0;     // 8.1 SysTray running
volatile LONG g_started = 0;
HMODULE g_hBat = nullptr;       // kept loaded for the process lifetime
HMODULE g_hSto = nullptr;

// ------------------------------------------------------------ RAII helpers
class ScopedLibrary {
public:
	explicit ScopedLibrary(HMODULE h = nullptr) : m_h(h) {}
	~ScopedLibrary() { if (m_h) FreeLibrary(m_h); }
	HMODULE Get() const { return m_h; }
	HMODULE Release() { HMODULE h = m_h; m_h = nullptr; return h; }
private:
	HMODULE m_h;
	ScopedLibrary(const ScopedLibrary&) = delete;
	ScopedLibrary& operator=(const ScopedLibrary&) = delete;
};

class HeapBuffer {
public:
	explicit HeapBuffer(SIZE_T cb) : m_p(cb ? HeapAlloc(GetProcessHeap(), 0, cb) : nullptr), m_cb(cb) {}
	~HeapBuffer() { if (m_p) HeapFree(GetProcessHeap(), 0, m_p); }
	BYTE* Get() const { return (BYTE*)m_p; }
	SIZE_T Size() const { return m_cb; }
private:
	void* m_p; SIZE_T m_cb;
	HeapBuffer(const HeapBuffer&) = delete;
	HeapBuffer& operator=(const HeapBuffer&) = delete;
};

// Deletes the file on scope exit unless Commit() was called.
class TempFile {
public:
	explicit TempFile(const wchar_t* path) { StringCchCopyW(m_path, MAX_PATH, path); m_keep = false; }
	~TempFile() { if (!m_keep) DeleteFileW(m_path); }
	void Commit() { m_keep = true; }
	const wchar_t* Path() const { return m_path; }
private:
	wchar_t m_path[MAX_PATH]; bool m_keep;
	TempFile(const TempFile&) = delete;
	TempFile& operator=(const TempFile&) = delete;
};

// ------------------------------------------------------------ logging
void Log(LPCWSTR fmt, ...)
{
	wchar_t msg[1024];
	va_list ap; va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg), fmt, ap);
	va_end(ap);
	wchar_t line[1100];
	wnsprintfW(line, ARRAYSIZE(line), L"[ex7][w81] %s", msg);
	OutputDebugStringW(line);

	if (!g_cacheDir[0]) return;
	wchar_t path[MAX_PATH];
	wnsprintfW(path, MAX_PATH, L"%s\\w81flyout.log", g_cacheDir);
	ScopedHandle h(CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (h.Get() == INVALID_HANDLE_VALUE) return;
	LARGE_INTEGER sz = {};
	if (GetFileSizeEx(h.Get(), &sz) && sz.QuadPart > 256 * 1024) return;
	SYSTEMTIME st; GetLocalTime(&st);
	wchar_t full[1200];
	wnsprintfW(full, ARRAYSIZE(full), L"%04u-%02u-%02u %02u:%02u:%02u [%u] %s\r\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentThreadId(), msg);
	char utf8[3600];
	int n = WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, sizeof(utf8), nullptr, nullptr);
	DWORD w = 0;
	if (n > 1) WriteFile(h.Get(), utf8, (DWORD)(n - 1), &w, nullptr);
}

// ------------------------------------------------------------ registry
DWORD ReadDword(const wchar_t* name, DWORD def)
{
	DWORD v = def, cb = sizeof(v), type = 0;
	ScopedRegKey k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_READ, k.Put()) == ERROR_SUCCESS &&
		RegQueryValueExW(k.Get(), name, nullptr, &type, (LPBYTE)&v, &cb) == ERROR_SUCCESS && type == REG_DWORD)
		return v;
	return def;
}

bool ReadString(const wchar_t* name, wchar_t* out, DWORD cch)
{
	ScopedRegKey k;
	DWORD cb = (cch - 1) * sizeof(wchar_t), type = 0;
	out[0] = 0;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_READ, k.Put()) != ERROR_SUCCESS) return false;
	if (RegQueryValueExW(k.Get(), name, nullptr, &type, (LPBYTE)out, &cb) != ERROR_SUCCESS || type != REG_SZ) {
		out[0] = 0; return false;
	}
	out[cb / sizeof(wchar_t)] = 0;
	return out[0] != 0;
}

ULONGLONG NowFileTime()
{
	FILETIME ft; GetSystemTimeAsFileTime(&ft);
	return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

ULONGLONG ReadQword(const wchar_t* name)
{
	ULONGLONG v = 0; DWORD cb = sizeof(v), type = 0;
	ScopedRegKey k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_READ, k.Put()) == ERROR_SUCCESS)
		RegQueryValueExW(k.Get(), name, nullptr, &type, (LPBYTE)&v, &cb);
	return type == REG_QWORD ? v : 0;
}

void WriteQword(const wchar_t* name, ULONGLONG v)
{
	ScopedRegKey k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, k.Put(), nullptr) == ERROR_SUCCESS)
		RegSetValueExW(k.Get(), name, 0, REG_QWORD, (const BYTE*)&v, sizeof(v));
}

// ------------------------------------------------------------ config
bool ParseHexDword(const wchar_t* s, int len, DWORD* out)
{
	DWORD v = 0;
	for (int i = 0; i < len; ++i) {
		wchar_t c = s[i]; int d;
		if (c >= L'0' && c <= L'9') d = c - L'0';
		else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
		else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
		else return false;
		v = (v << 4) | (DWORD)d;
	}
	*out = v; return true;
}

bool IsHex64(const char* s)
{
	for (int i = 0; i < 64; ++i) {
		char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return s[64] == 0;
}

// Loads the package table, applying registry overrides for another 8.1 build.
void LoadConfig()
{
	static const wchar_t* idVals[2] = { L"W81BatmeterId", L"W81StobjectId" };
	static const wchar_t* shaVals[2] = { L"W81BatmeterSha256", L"W81StobjectSha256" };
	for (int i = 0; i < 2; ++i) {
		PackageCfg& c = g_pkg[i];
		StringCchCopyW(c.name, ARRAYSIZE(c.name), kDefault[i].name);
		StringCchCopyW(c.id, ARRAYSIZE(c.id), kDefault[i].id);
		c.tds = kDefault[i].timeDateStamp; c.soi = kDefault[i].sizeOfImage;
		StringCchCopyA(c.sha, ARRAYSIZE(c.sha), kDefault[i].sha256);

		wchar_t id[32], sha[80];
		bool hasId = ReadString(idVals[i], id, ARRAYSIZE(id));
		bool hasSha = ReadString(shaVals[i], sha, ARRAYSIZE(sha));
		if (hasId != hasSha) { Log(L"override for %s needs both id and sha256; ignored", c.name); continue; }
		if (!hasId) continue;
		int len = lstrlenW(id);
		DWORD tds = 0, soi = 0;
		char shaA[80] = {};
		for (int k = 0; k < 64 && sha[k]; ++k) {
			wchar_t ch = sha[k];
			shaA[k] = (char)((ch >= L'A' && ch <= L'F') ? ch + 32 : ch);
		}
		if (len < 9 || len > 16 || !ParseHexDword(id, 8, &tds) || !ParseHexDword(id + 8, len - 8, &soi) ||
			lstrlenW(sha) != 64 || !IsHex64(shaA)) {
			Log(L"invalid override for %s (id=%s); using default", c.name, id);
			continue;
		}
		StringCchCopyW(c.id, ARRAYSIZE(c.id), id);
		c.tds = tds; c.soi = soi;
		StringCchCopyA(c.sha, ARRAYSIZE(c.sha), shaA);
		Log(L"override %s: id=%s", c.name, id);
	}
}

bool BuildSupported()
{
	DWORD b = g_osVersion.BuildNumber();
	for (const BuildRange& r : kBuilds) {
		if (b >= r.minBuild && b <= r.maxBuild) {
			if (!r.supported && ReadDword(L"W81FlyoutForce", 0) == 1) {
				Log(L"build %u (%s) outside the table, forced by W81FlyoutForce", b, r.note);
				return true;
			}
			if (!r.supported) Log(L"build %u (%s): disabled (W81FlyoutForce=1 to try)", b, r.note);
			return r.supported;
		}
	}
	Log(L"build %u: not in table, disabled", b);
	return false;
}

bool HasBattery()
{
	SYSTEM_POWER_STATUS ps;
	return GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
}

bool InitCacheDir()
{
	wchar_t base[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (!n || n >= MAX_PATH - 96) return false;
	wchar_t dir[MAX_PATH];
	wnsprintfW(dir, MAX_PATH, L"%s\\7explorer", base);
	CreateDirectoryW(dir, nullptr);
	wnsprintfW(dir, MAX_PATH, L"%s\\7explorer\\w81flyout", base);
	CreateDirectoryW(dir, nullptr);
	wnsprintfW(g_cacheDir, MAX_PATH, L"%s\\%s-%s", dir, g_pkg[1].id, g_pkg[0].id);
	CreateDirectoryW(g_cacheDir, nullptr);
	return GetFileAttributesW(g_cacheDir) != INVALID_FILE_ATTRIBUTES;
}

void PkgPath(int i, wchar_t* out, const wchar_t* suffix)
{
	wnsprintfW(out, MAX_PATH, L"%s\\%s%s", g_cacheDir, g_pkg[i].name, suffix);
}

// ------------------------------------------------------------ verification
typedef NTSTATUS(WINAPI* BCryptOpenAlgorithmProvider_t)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI* BCryptHash_t)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR, ULONG, PUCHAR, ULONG);
typedef NTSTATUS(WINAPI* BCryptCloseAlgorithmProvider_t)(BCRYPT_ALG_HANDLE, ULONG);

// Reads a whole (small) file into a heap buffer owned by the caller's RAII.
bool ReadWholeFile(const wchar_t* path, HeapBuffer** out, DWORD* size)
{
	*out = nullptr; *size = 0;
	ScopedHandle f(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (f.Get() == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz;
	if (!GetFileSizeEx(f.Get(), &sz) || sz.QuadPart <= 0 || sz.QuadPart > 32 * 1024 * 1024) return false;
	HeapBuffer* buf = new HeapBuffer((SIZE_T)sz.QuadPart);
	DWORD rd = 0;
	if (!buf || !buf->Get() || !ReadFile(f.Get(), buf->Get(), (DWORD)sz.QuadPart, &rd, nullptr) || rd != (DWORD)sz.QuadPart) {
		delete buf; return false;
	}
	*out = buf; *size = rd;
	return true;
}

class OwnedBuffer {  // RAII for ReadWholeFile results
public:
	OwnedBuffer() : m_b(nullptr), m_n(0) {}
	~OwnedBuffer() { delete m_b; }
	bool Load(const wchar_t* path) { delete m_b; m_b = nullptr; return ReadWholeFile(path, &m_b, &m_n); }
	BYTE* Data() const { return m_b ? m_b->Get() : nullptr; }
	DWORD Size() const { return m_n; }
private:
	HeapBuffer* m_b; DWORD m_n;
	OwnedBuffer(const OwnedBuffer&) = delete;
	OwnedBuffer& operator=(const OwnedBuffer&) = delete;
};

bool Sha256File(const wchar_t* path, char hex[65])
{
	hex[0] = 0;
	OwnedBuffer buf;
	if (!buf.Load(path)) return false;
	ScopedLibrary bc(LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!bc.Get()) return false;
	auto pOpen = (BCryptOpenAlgorithmProvider_t)GetProcAddress(bc.Get(), "BCryptOpenAlgorithmProvider");
	auto pHash = (BCryptHash_t)GetProcAddress(bc.Get(), "BCryptHash");
	auto pClose = (BCryptCloseAlgorithmProvider_t)GetProcAddress(bc.Get(), "BCryptCloseAlgorithmProvider");
	if (!pOpen || !pHash || !pClose) return false;
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (pOpen(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
	UCHAR digest[32];
	NTSTATUS st = pHash(alg, nullptr, 0, buf.Data(), buf.Size(), digest, sizeof(digest));
	pClose(alg, 0);
	if (st != 0) return false;
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 32; ++i) { hex[i * 2] = hx[digest[i] >> 4]; hex[i * 2 + 1] = hx[digest[i] & 15]; }
	hex[64] = 0;
	return true;
}

// PE sanity: x64 DLL with the expected TimeDateStamp / SizeOfImage.
bool CheckPeHeader(const wchar_t* path, DWORD tds, DWORD soi)
{
	ScopedHandle f(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (f.Get() == INVALID_HANDLE_VALUE) return false;
	BYTE hdr[1024]; DWORD rd = 0;
	if (!ReadFile(f.Get(), hdr, sizeof(hdr), &rd, nullptr) || rd < sizeof(IMAGE_DOS_HEADER)) return false;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)hdr;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
		(DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > rd) return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(hdr + dos->e_lfanew);
	return nt->Signature == IMAGE_NT_SIGNATURE &&
		nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
		(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) &&
		nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
		nt->FileHeader.TimeDateStamp == tds &&
		nt->OptionalHeader.SizeOfImage == soi;
}

// Authenticode is checked and logged but cannot be the gate: the files are
// catalog-signed only (CI: Get-AuthenticodeSignature = NotSigned).
void LogAuthenticode(const wchar_t* path)
{
	typedef LONG(WINAPI* WinVerifyTrust_t)(HWND, GUID*, LPVOID);
	ScopedLibrary wt(LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	auto pWVT = wt.Get() ? (WinVerifyTrust_t)GetProcAddress(wt.Get(), "WinVerifyTrust") : nullptr;
	if (!pWVT) return;
	GUID action = { 0x00aac56b, 0xcd44, 0x11d0, { 0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee } }; // GENERIC_VERIFY_V2
	struct FileInfo { DWORD cbStruct; LPCWSTR path; HANDLE hFile; GUID* subject; } fi = { sizeof(FileInfo), path, nullptr, nullptr };
	struct TrustData {
		DWORD cbStruct; LPVOID policy; LPVOID sip; DWORD uiChoice; DWORD revocation; DWORD unionChoice;
		FileInfo* file; DWORD stateAction; HANDLE state; WCHAR* url; DWORD provFlags; DWORD uiContext; LPVOID sigSettings;
	} td = {};
	td.cbStruct = sizeof(td); td.uiChoice = 2; /* WTD_UI_NONE */ td.unionChoice = 1; /* WTD_CHOICE_FILE */
	td.file = &fi; td.provFlags = 0x1000; /* WTD_CACHE_ONLY_URL_RETRIEVAL */
	LONG r = pWVT((HWND)INVALID_HANDLE_VALUE, &action, &td);
	Log(L"Authenticode %s: 0x%08X%s", path, (DWORD)r,
		r == (LONG)0x800B0100 ? L" (no embedded signature: expected for catalog-signed system files; SHA-256 pin is the gate)" : L"");
}

bool VerifyPackageFile(int i, const wchar_t* path)
{
	if (!CheckPeHeader(path, g_pkg[i].tds, g_pkg[i].soi)) {
		Log(L"%s: PE header mismatch (machine/TimeDateStamp/SizeOfImage)", path);
		return false;
	}
	char hex[65];
	if (!Sha256File(path, hex)) { Log(L"%s: cannot hash", path); return false; }
	if (lstrcmpA(hex, g_pkg[i].sha) != 0) {
		wchar_t wh[65];
		MultiByteToWideChar(CP_ACP, 0, hex, -1, wh, 65);
		Log(L"%s: SHA-256 mismatch (%s)", path, wh);
		return false;
	}
	return true;
}

// ------------------------------------------------------------ download
typedef HRESULT(WINAPI* URLDownloadToFileW_t)(LPUNKNOWN, LPCWSTR, LPCWSTR, DWORD, LPVOID);

bool DownloadPackage(int i)
{
	wchar_t url[256], part[MAX_PATH], final_[MAX_PATH];
	wnsprintfW(url, ARRAYSIZE(url), L"https://msdl.microsoft.com/download/symbols/%s/%s/%s",
		g_pkg[i].name, g_pkg[i].id, g_pkg[i].name);
	PkgPath(i, part, L".part");
	PkgPath(i, final_, L"");

	ScopedLibrary urlmon(LoadLibraryExW(L"urlmon.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	auto pDl = urlmon.Get() ? (URLDownloadToFileW_t)GetProcAddress(urlmon.Get(), "URLDownloadToFileW") : nullptr;
	if (!pDl) { Log(L"urlmon unavailable"); return false; }

	DeleteFileW(part);
	TempFile tmp(part);              // deleted on every failure path
	HRESULT hr = pDl(nullptr, url, part, 0, nullptr);
	if (FAILED(hr)) {
		// 404 -> INET_E_RESOURCE_NOT_FOUND/INET_E_DOWNLOAD_FAILURE; offline -> INET_E_CANNOT_CONNECT etc.
		Log(L"download %s failed hr=0x%08X", url, (DWORD)hr);
		return false;
	}
	if (!VerifyPackageFile(i, part)) return false;
	LogAuthenticode(part);
	if (!MoveFileExW(part, final_, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		Log(L"move to %s failed (%u)", final_, GetLastError());
		return false;
	}
	tmp.Commit();                    // .part no longer exists
	Log(L"downloaded and verified %s", final_);
	return true;
}

// ------------------------------------------------------------ .mui re-keying
bool RvaToOffset(const IMAGE_SECTION_HEADER* sec, WORD nsec, DWORD rva, DWORD fileSize, DWORD* off)
{
	for (WORD s = 0; s < nsec; ++s) {
		DWORD va = sec[s].VirtualAddress;
		DWORD vs = sec[s].Misc.VirtualSize > sec[s].SizeOfRawData ? sec[s].Misc.VirtualSize : sec[s].SizeOfRawData;
		if (rva >= va && rva < va + vs) {
			DWORD o = sec[s].PointerToRawData + (rva - va);
			if (o >= fileSize) return false;
			*off = o; return true;
		}
	}
	return false;
}

// File offset/size of resource type "MUI" (first name, first language).
// Every access is bounds-checked; the caller also runs this under SEH.
bool FindMuiResource(const BYTE* file, DWORD size, DWORD* offOut, DWORD* sizeOut)
{
	if (size < sizeof(IMAGE_DOS_HEADER)) return false;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)file;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
		(DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size) return false;
	const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(file + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
	DWORD rsrcRva = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		? ((const IMAGE_NT_HEADERS64*)nt)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress
		: nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	WORD nsec = nt->FileHeader.NumberOfSections;
	if (!rsrcRva || (const BYTE*)(sec + nsec) > file + size) return false;
	DWORD root;
	if (!RvaToOffset(sec, nsec, rsrcRva, size, &root)) return false;

	auto dirAt = [&](DWORD rel, const IMAGE_RESOURCE_DIRECTORY** d) -> bool {
		if (root + rel + sizeof(IMAGE_RESOURCE_DIRECTORY) > size) return false;
		*d = (const IMAGE_RESOURCE_DIRECTORY*)(file + root + rel);
		DWORD n = (DWORD)(*d)->NumberOfNamedEntries + (*d)->NumberOfIdEntries;
		return root + rel + sizeof(IMAGE_RESOURCE_DIRECTORY) + n * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY) <= size;
	};
	const IMAGE_RESOURCE_DIRECTORY* d0;
	if (!dirAt(0, &d0)) return false;
	const IMAGE_RESOURCE_DIRECTORY_ENTRY* e = (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(d0 + 1);
	for (DWORD i = 0; i < d0->NumberOfNamedEntries; ++i) {
		if (!e[i].NameIsString || !e[i].DataIsDirectory) continue;
		DWORD no = root + e[i].NameOffset;
		if (no + 2 + 6 > size) continue;
		const WORD* nm = (const WORD*)(file + no);
		if (nm[0] != 3 || nm[1] != L'M' || nm[2] != L'U' || nm[3] != L'I') continue;
		const IMAGE_RESOURCE_DIRECTORY* d1; const IMAGE_RESOURCE_DIRECTORY* d2;
		if (!dirAt(e[i].OffsetToDirectory, &d1) || d1->NumberOfNamedEntries + d1->NumberOfIdEntries == 0) return false;
		const IMAGE_RESOURCE_DIRECTORY_ENTRY* e1 = (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(d1 + 1);
		if (!e1->DataIsDirectory || !dirAt(e1->OffsetToDirectory, &d2) || d2->NumberOfNamedEntries + d2->NumberOfIdEntries == 0) return false;
		const IMAGE_RESOURCE_DIRECTORY_ENTRY* e2 = (const IMAGE_RESOURCE_DIRECTORY_ENTRY*)(d2 + 1);
		if (e2->DataIsDirectory || root + e2->OffsetToData + sizeof(IMAGE_RESOURCE_DATA_ENTRY) > size) return false;
		const IMAGE_RESOURCE_DATA_ENTRY* de = (const IMAGE_RESOURCE_DATA_ENTRY*)(file + root + e2->OffsetToData);
		DWORD off;
		if (!RvaToOffset(sec, nsec, de->OffsetToData, size, &off) || off + de->Size > size) return false;
		*offOut = off; *sizeOut = de->Size;
		return true;
	}
	return false;
}

// MUI resource layout (MS-internal but stable since Vista): signature
// 0xFECDFECD at +0, file type at +0x10 (0x11 LN, 0x12 MUI), service checksum
// at +0x1C and main checksum at +0x2C (16 bytes each). The loader accepts a
// .mui whose checksums equal the LN file's (verified on CI, 24H2 en-US).
const DWORD kMuiSig = 0xFECDFECD;

struct RekeyCtx { const wchar_t* ln; const wchar_t* src; const wchar_t* dst; bool ok; };

void RekeyMuiUnsafe(RekeyCtx* c)
{
	OwnedBuffer ln, mui;
	if (!ln.Load(c->ln) || !mui.Load(c->src)) return;
	DWORD lnOff, lnSize, mOff, mSize;
	if (!FindMuiResource(ln.Data(), ln.Size(), &lnOff, &lnSize) || lnSize < 0x3C) return;
	if (!FindMuiResource(mui.Data(), mui.Size(), &mOff, &mSize) || mSize < 0x3C) return;
	const BYTE* l = ln.Data() + lnOff; BYTE* m = mui.Data() + mOff;
	if (*(const DWORD*)l != kMuiSig || *(const DWORD*)m != kMuiSig) return;
	if (*(const DWORD*)(l + 0x10) != 0x11 || *(const DWORD*)(m + 0x10) != 0x12) return;
	memcpy(m + 0x1C, l + 0x1C, 0x20);

	wchar_t tmpPath[MAX_PATH];
	wnsprintfW(tmpPath, MAX_PATH, L"%s.part", c->dst);
	TempFile tmp(tmpPath);
	{
		ScopedHandle f(CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		DWORD w = 0;
		if (f.Get() == INVALID_HANDLE_VALUE || !WriteFile(f.Get(), mui.Data(), mui.Size(), &w, nullptr) || w != mui.Size())
			return;
	}
	if (!MoveFileExW(tmpPath, c->dst, MOVEFILE_REPLACE_EXISTING)) return;
	tmp.Commit();
	c->ok = true;
}

// Builds <cache>\<lang>\<name>.mui from %SystemRoot%\System32\<lang>\<name>.mui.
bool PrepareMui(int i, const wchar_t* lang)
{
	wchar_t sys[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH], ln[MAX_PATH], win[MAX_PATH];
	if (!GetSystemDirectoryW(win, MAX_PATH)) return false;
	wnsprintfW(sys, MAX_PATH, L"%s\\%s\\%s.mui", win, lang, g_pkg[i].name);
	if (GetFileAttributesW(sys) == INVALID_FILE_ATTRIBUTES) return false;
	wnsprintfW(dir, MAX_PATH, L"%s\\%s", g_cacheDir, lang);
	CreateDirectoryW(dir, nullptr);
	wnsprintfW(dst, MAX_PATH, L"%s\\%s.mui", dir, g_pkg[i].name);
	PkgPath(i, ln, L"");
	RekeyCtx c = { ln, sys, dst, false };
	if (!SafeInvokeCtx<RekeyCtx>(L"RekeyMui", RekeyMuiUnsafe, &c)) c.ok = false;
	Log(L"mui %s -> %s: %s", sys, dst, c.ok ? L"ok" : L"FAILED");
	return c.ok;
}

// Preferred UI languages of the user + en-US (the loader falls back to it).
bool PrepareAllMui()
{
	wchar_t langs[512]; ULONG n = 0, cch = ARRAYSIZE(langs);
	bool any = false;
	if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &n, langs, &cch)) {
		for (const wchar_t* p = langs; *p; p += lstrlenW(p) + 1) {
			bool a = PrepareMui(0, p), b = PrepareMui(1, p);
			any = any || (a && b);
		}
	}
	bool a = PrepareMui(0, L"en-US"), b = PrepareMui(1, L"en-US");
	return any || (a && b);
}

// ------------------------------------------------------------ worker
bool CacheComplete(bool verify)
{
	for (int i = 0; i < 2; ++i) {
		wchar_t p[MAX_PATH];
		PkgPath(i, p, L"");
		if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return false;
		if (verify && !VerifyPackageFile(i, p)) {
			Log(L"cached %s failed verification: deleting", p);
			DeleteFileW(p);
			return false;
		}
	}
	return true;
}

void PrepareUnsafe()
{
	if (CacheComplete(true)) {
		PrepareAllMui();               // system .mui may have been serviced: re-key every start
		InterlockedExchange(&g_ready, 1);
		Log(L"cache ready (%s)", g_cacheDir);
		return;
	}
	// One attempt per 24 h after a failure; never loop.
	ULONGLONG last = ReadQword(L"W81FlyoutLastFailure"), now = NowFileTime();
	if (last && now > last && now - last < 24ULL * 3600 * 10000000ULL) {
		Log(L"previous download failed less than 24h ago: skipping this session");
		return;
	}
	bool ok = true;
	for (int i = 0; i < 2 && ok; ++i) {
		wchar_t p[MAX_PATH];
		PkgPath(i, p, L"");
		if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES)
			ok = DownloadPackage(i);
	}
	if (!ok || !PrepareAllMui()) {
		WriteQword(L"W81FlyoutLastFailure", now);
		Log(L"preparation failed: legacy flyout disabled, system behaviour kept");
		return;
	}
	WriteQword(L"W81FlyoutLastFailure", 0);
	InterlockedExchange(&g_ready, 1);
	Log(L"cache ready after download; the 8.1 flyout is used from the next explorer start");
}

DWORD WINAPI WorkerThread(LPVOID)
{
	HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // urlmon
	SafeInvoke(L"w81 prepare", PrepareUnsafe);
	if (SUCCEEDED(hrCo)) CoUninitialize();
	return 0;
}

// ------------------------------------------------------------ loading
typedef HWND(WINAPI* CreateWindowInBand_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD);
CreateWindowInBand_t g_origCWIB = nullptr;

// The 8.1 flyout window is created with CreateWindowInBand; bands can be
// refused on 10/11 (ERROR_ACCESS_DENIED). Fall back to a normal top-most
// window so the flyout still appears.
HWND WINAPI CreateWindowInBand_W81(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
	HWND parent, HMENU menu, HINSTANCE inst, LPVOID param, DWORD band)
{
	HWND r = g_origCWIB ? g_origCWIB(ex, cls, name, style, x, y, w, h, parent, menu, inst, param, band) : nullptr;
	if (!r) {
		DWORD err = GetLastError();
		r = CreateWindowExW(ex | WS_EX_TOPMOST, cls, name, style, x, y, w, h, parent, menu, inst, param);
		Log(L"CreateWindowInBand(band %u) failed (%u): CreateWindowExW fallback -> %p", band, err, r);
	}
	return r;
}

struct CreateCtx { LPUNKNOWN outer; const IID* riid; void** ppv; HRESULT hr; bool ok; };

typedef HRESULT(STDAPICALLTYPE* DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);

void CreateUnsafe(CreateCtx* c)
{
	if (!g_ready && !CacheComplete(false)) { Log(L"cache not ready: system SysTray this session"); return; }
	wchar_t bat[MAX_PATH], sto[MAX_PATH];
	PkgPath(0, bat, L""); PkgPath(1, sto, L"");
	// Re-verify right before loading (the cache is user-writable).
	if (!VerifyPackageFile(0, bat) || !VerifyPackageFile(1, sto)) return;

	// stobject imports "BatMeter.dll" by name: if the system copy is already
	// in the process the loader would bind to it. Refuse in that case.
	HMODULE existing = GetModuleHandleW(L"batmeter.dll");
	if (existing) {
		wchar_t p[MAX_PATH] = {};
		GetModuleFileNameW(existing, p, MAX_PATH);
		if (lstrcmpiW(p, bat) != 0) { Log(L"system batmeter already loaded (%s): not loading 8.1", p); return; }
	}
	if (GetModuleHandleW(L"stobject.dll")) { Log(L"a stobject.dll is already loaded: not loading 8.1"); return; }

	ScopedLibrary hBat(LoadLibraryExW(bat, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
	if (!hBat.Get()) { Log(L"LoadLibrary %s failed (%u)", bat, GetLastError()); return; }
	ScopedLibrary hSto(LoadLibraryExW(sto, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
	if (!hSto.Get()) { Log(L"LoadLibrary %s failed (%u)", sto, GetLastError()); return; }

	HMODULE bound = GetModuleHandleW(L"batmeter.dll");
	wchar_t bp[MAX_PATH] = {};
	GetModuleFileNameW(bound, bp, MAX_PATH);
	if (lstrcmpiW(bp, bat) != 0) { Log(L"stobject bound to %s instead of the 8.1 batmeter", bp); return; }

	HMODULE u32 = GetModuleHandleW(L"user32.dll");
	g_origCWIB = (CreateWindowInBand_t)GetProcAddress(u32, "CreateWindowInBand");
	if (g_origCWIB)
		Log(L"CreateWindowInBand IAT patch: %d",
			ChangeImportedAddress(hSto.Get(), "USER32.dll", g_origCWIB, CreateWindowInBand_W81));

	auto pGet = (DllGetClassObject_t)GetProcAddress(hSto.Get(), "DllGetClassObject");
	if (!pGet) { Log(L"DllGetClassObject missing"); return; }
	ComPtr<IClassFactory> cf;
	HRESULT hr = pGet(kClsidSysTray, IID_IClassFactory, cf.PutVoid());
	if (FAILED(hr) || !cf) { Log(L"DllGetClassObject hr=0x%08X", (DWORD)hr); return; }
	hr = cf->CreateInstance(c->outer, *c->riid, c->ppv);
	if (FAILED(hr) || !*c->ppv) { Log(L"CreateInstance hr=0x%08X", (DWORD)hr); return; }

	// Success: keep both modules for the life of the process (the object
	// lives in them; FreeLibrary would unmap live code).
	g_hBat = hBat.Release();
	g_hSto = hSto.Release();
	c->hr = hr;
	c->ok = true;
	InterlockedExchange(&g_active, 1);
	Log(L"8.1 SysTray created from %s", g_cacheDir);
}

} // namespace

// ------------------------------------------------------------ public
void StartBackgroundPrepare()
{
	if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
	if (ReadDword(L"LegacyBatteryFlyout", 0) == 0) return;
	LoadConfig();
	if (!InitCacheDir()) return;
	if (!BuildSupported()) return;
	if (!HasBattery()) { Log(L"no battery: nothing to do"); return; }
	HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
	if (h) CloseHandle(h);
	else Log(L"CreateThread failed (%u)", GetLastError());
}

bool TryCreateSysTray(LPUNKNOWN pUnkOuter, REFIID riid, void** ppv, HRESULT* phr)
{
	// test15: opt-in (LegacyBatteryFlyout=1). Default path is the Win32
	// flyout of the system stobject (ShellFixes.cpp, UseWin32BatteryFlyout).
	if (ReadDword(L"LegacyBatteryFlyout", 0) == 0) { Log(L"SysTray: 8.1 package not requested (LegacyBatteryFlyout=0), system stobject"); return false; }
	if (!g_started || !g_cacheDir[0] || !ppv) { Log(L"SysTray: 8.1 skipped, started=%d cache=%s", g_started, g_cacheDir); return false; }
	if (!BuildSupported()) { Log(L"SysTray: 8.1 skipped, build not supported"); return false; }
	if (!HasBattery()) { Log(L"SysTray: 8.1 skipped, no battery"); return false; }
	*ppv = nullptr;
	CreateCtx c = { pUnkOuter, &riid, ppv, E_FAIL, false };
	if (!SafeInvokeCtx<CreateCtx>(L"w81 CreateSysTray", CreateUnsafe, &c)) {
		c.ok = false;
		*ppv = nullptr;   // object state unknown after a fault: do not hand it out
	}
	if (!c.ok) return false;
	*phr = c.hr;
	return true;
}

bool IsActive() { return g_active != 0; }

bool WrapSysTray() { return ReadDword(L"W81SysTrayWrapper", 1) != 0; }

bool Sha256OfFile(const wchar_t* path, char hex[65]) { return Sha256File(path, hex); }

} // namespace w81
} // namespace ex7
