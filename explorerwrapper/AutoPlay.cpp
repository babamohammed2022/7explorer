// AutoPlay support: a thin IAutoPlayUI pass-through plus per-user defaults repair.
//
// The wrapper (installed by Shell32_CoCreateInstance in shell32_wrappers.cpp)
// forwards every call to the real system AutoPlay object; InitVolumeAutoplay
// ORs in flag 0x2 (content-sniffing hint, Explorer7-original behavior).
//
// EnsureAutoPlayDefaults (test39, inspired by the win7-classic-autoplay-restorer
// Windhawk mod) repairs only states that fully BREAK AutoPlay, HKCU-only:
//   * Policies\Explorer\NoDriveTypeAutoRun present and 0xFF (everything disabled)
//     -> restored to the Wine default 0x91 used by the mod;
//   * Policies\Explorer\NoAutoplayfornonVolume present and != 0 -> restored to 0.
// Missing values are left alone (the system default already applies) and HKLM is
// never touched. Opt-out: Advanced\FixAutoPlay = 0. Deliberately NOT ported from
// the mod: theme/owner-draw patches, MTP/media guards and dialog-lifetime hooks -
// they need deep shell32 patching, and the "Open folder" entry removal is exactly
// what the mod author regretted upstream.
#include "AutoPlay.h"
#include "dbgprint.h"

namespace {

// RAII registry key (closes on scope exit, never leaks on early return).
struct AutoPlayRegKey {
	HKEY h;
	AutoPlayRegKey() : h(nullptr) {}
	~AutoPlayRegKey() { if (h) RegCloseKey(h); }
	HKEY* Put() { return &h; }
	HKEY Get() const { return h; }
};

DWORD ReadAdvancedDwordLocal(const wchar_t* name, DWORD fallback)
{
	DWORD v = fallback, cb = sizeof(v), type = 0;
	if (RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
		name, RRF_RT_REG_DWORD, &type, &v, &cb) == ERROR_SUCCESS && type == REG_DWORD)
		return v;
	return fallback;
}

} // namespace

CAutoPlayWrapper::CAutoPlayWrapper(IAutoPlayUI *autoui)
{
	m_cRef = 1;
	m_autoui = autoui;
}

CAutoPlayWrapper::~CAutoPlayWrapper()
{
	if (m_autoui) m_autoui->Release();
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::QueryInterface(REFIID riid,void **ppvObject)
{
	return m_autoui->QueryInterface(riid,ppvObject);
}

ULONG STDMETHODCALLTYPE CAutoPlayWrapper::AddRef(void)
{
	return InterlockedIncrement(&m_cRef);
}

ULONG STDMETHODCALLTYPE CAutoPlayWrapper::Release(void)
{
	if (InterlockedDecrement(&m_cRef) == 0)
	{
		delete this;
		return 0;
	}
	return m_cRef;
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitVolumeAutoplay(IUnknown * p1,LPCWSTR p2,LPCWSTR p3,ULONG p4,ULONG p5,ULONG p6,LPCWSTR p7,LPCWSTR p8,int p9,LPCWSTR p10,LPCWSTR p11,HWND p12)
{
	dbgprintf(L"CAutoPlayWrapper::InitVolumeAutoplay flags=0x%lx\n", (unsigned long)(p6 | 2));
	p6 = p6 | 2;
	return m_autoui->InitVolumeAutoplay(p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,p12);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitNoContentAutoplay(IUnknown * p1,REFGUID p2,LPCWSTR p3,ULONG p4,int p5,LPCWSTR p6,LPCWSTR p7,LPCWSTR p8)
{
	return m_autoui->InitNoContentAutoplay(p1,p2,p3,p4,p5,p6,p7,p8);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitDirectAutoPlay(IUnknown * p1,LPCWSTR p2,HWND p3)
{
	return m_autoui->InitDirectAutoPlay(p1,p2,p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::ToastPromptForChkDsk(LPCWSTR p1,int * p2,int * p3)
{
	return m_autoui->ToastPromptForChkDsk(p1,p2,p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::LaunchDeviceHandler(LPCWSTR p1,LPCWSTR p2,LPCWSTR p3)
{
	return m_autoui->LaunchDeviceHandler(p1,p2,p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::IsDialogClosed(void)
{
	return m_autoui->IsDialogClosed();
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::SniffComplete(ULONG p1)
{
	return m_autoui->SniffComplete(p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::CloseDialog(void)
{
	return m_autoui->CloseDialog();
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::AddContentType(ULONG p1)
{
	return m_autoui->AddContentType(p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::MoreInterfaceArrived(LPCWSTR p1)
{
	return m_autoui->MoreInterfaceArrived(p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::SetChkDskCompleted(void)
{
	return m_autoui->SetChkDskCompleted();
}

// Repairs per-user AutoPlay policies that fully disable AutoPlay (see the file
// header). HKCU-only, missing values untouched, FixAutoPlay = 0 opts out.
void EnsureAutoPlayDefaults()
{
	try {
		if (ReadAdvancedDwordLocal(L"FixAutoPlay", 1) == 0) {
			dbgprintf(L"EnsureAutoPlayDefaults: disabled (FixAutoPlay=0)\n");
			return;
		}
		AutoPlayRegKey pol;
		LSTATUS o = RegOpenKeyExW(HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
			0, KEY_QUERY_VALUE | KEY_SET_VALUE, pol.Put());
		if (o != ERROR_SUCCESS) {
			dbgprintf(L"EnsureAutoPlayDefaults: no per-user Policies\\Explorer key, nothing to repair\n");
			return; // key absent: nothing to repair, and we do not create it
		}
		DWORD v = 0, cb = sizeof(v), type = 0;
		if (RegQueryValueExW(pol.Get(), L"NoDriveTypeAutoRun", nullptr, &type, (BYTE*)&v, &cb) == ERROR_SUCCESS &&
			type == REG_DWORD && v == 0xFF) {
			v = 0x91; // Wine/Win7 default: unknown + remote + reserved disabled
			if (RegSetValueExW(pol.Get(), L"NoDriveTypeAutoRun", 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS)
				dbgprintf(L"EnsureAutoPlayDefaults: NoDriveTypeAutoRun 0xFF -> 0x91\n");
		}
		v = 0; cb = sizeof(v); type = 0;
		if (RegQueryValueExW(pol.Get(), L"NoAutoplayfornonVolume", nullptr, &type, (BYTE*)&v, &cb) == ERROR_SUCCESS &&
			type == REG_DWORD && v != 0) {
			v = 0;
			if (RegSetValueExW(pol.Get(), L"NoAutoplayfornonVolume", 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS)
				dbgprintf(L"EnsureAutoPlayDefaults: NoAutoplayfornonVolume -> 0\n");
		}
	} catch (...) {
		dbgprintf(L"EnsureAutoPlayDefaults: exception, policies untouched\n");
	}
}
