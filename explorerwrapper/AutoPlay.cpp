// AutoPlay support for the private Win7 Explorer shell.
//
//  * CAutoPlayWrapper is a guarded pass-through to the native IAutoPlayUI.
//  * EnsureAutoPlayDefaults repairs only per-user policies that explicitly
//    disable AutoPlay completely (HKCU only; FixAutoPlay=0 opts out).
//  * A hidden top-level window observes basic volume-arrival messages and asks
//    the system shell to run the volume's registered "autoplay" verb. This is
//    intentionally best effort: it relies on the current Windows build exposing
//    that verb and on the user's AutoPlay policy/handlers.
#include "AutoPlay.h"
#include "SafeGuards.h"
#include "dbgprint.h"
#include <dbt.h>
#include <shellapi.h>

namespace {

const wchar_t kAdvancedKey[] =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
const wchar_t kPoliciesExplorerKey[] =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
const wchar_t kAutoPlayHandlersKey[] =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers";
const wchar_t kAutoPlayMonitorClass[] =
	L"Win7ExplorerRestorer.AutoPlayDeviceMonitor";
const UINT_PTR kAutoPlayRetryTimer = 0x742;
const DWORD kAutoPlayInitialDelayMs = 750;
const DWORD kAutoPlayRetryDelayMs = 350;
const DWORD kAutoPlayRetryLimit = 8;
const ULONGLONG kAutoPlayDuplicateWindowMs = 5000;

struct AutoPlayMonitorState {
	DWORD pendingVolumes;
	DWORD retryCount[26];
	ULONGLONG lastInvocation[26];
};

AutoPlayMonitorState g_autoPlayMonitorState = {};
volatile LONG g_autoPlayMonitorStarted = 0;

// Handles a native COM call that may cross into version-dependent shell code.
// Args used here are COM ABI scalars/references/pointers (no destructors), so
// this SEH boundary is safe with the project's /EH- build.
template <class Interface, class Method, class... Args>
static HRESULT SafeAutoPlayCall(const wchar_t* where, Interface* object,
	Method method, Args... args)
{
	if (!object) return E_NOINTERFACE;
	__try {
		return (object->*method)(args...);
	}
	__except (Win7ExplorerRestorer::SehFilter(where, GetExceptionInformation())) {
		return E_UNEXPECTED;
	}
}

static ULONG SafeAutoPlayRelease(IUnknown* object)
{
	if (!object) return 0;
	__try {
		return object->Release();
	}
	__except (Win7ExplorerRestorer::SehFilter(L"AutoPlay::Release", GetExceptionInformation())) {
		return 0;
	}
}

DWORD ReadAdvancedDwordLocal(const wchar_t* name, DWORD fallback)
{
	DWORD value = fallback, bytes = sizeof(value), type = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, kAdvancedKey, name,
		RRF_RT_REG_DWORD, &type, &value, &bytes) == ERROR_SUCCESS && type == REG_DWORD)
		return value;
	return fallback;
}

static bool ReadRegistryDword(HKEY root, const wchar_t* key,
	const wchar_t* name, DWORD* value)
{
	if (!value) return false;
	DWORD bytes = sizeof(*value), type = 0;
	return RegGetValueW(root, key, name, RRF_RT_REG_DWORD,
		&type, value, &bytes) == ERROR_SUCCESS && type == REG_DWORD;
}

static DWORD ReadPolicyMask(const wchar_t* name)
{
	DWORD value = 0, combined = 0;
	if (ReadRegistryDword(HKEY_CURRENT_USER, kPoliciesExplorerKey, name, &value))
		combined |= value;
	value = 0;
	if (ReadRegistryDword(HKEY_LOCAL_MACHINE, kPoliciesExplorerKey, name, &value))
		combined |= value;
	return combined;
}

static bool IsAutoPlayGloballyDisabled()
{
	static const wchar_t* const keys[] = {
		kPoliciesExplorerKey,
		kAutoPlayHandlersKey
	};
	const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
	for (size_t i = 0; i < ARRAYSIZE(roots); ++i) {
		for (size_t j = 0; j < ARRAYSIZE(keys); ++j) {
			DWORD disabled = 0;
			if (ReadRegistryDword(roots[i], keys[j], L"DisableAutoplay", &disabled) && disabled != 0)
				return true;
		}
	}
	return false;
}

static DWORD DriveTypePolicyBit(UINT driveType)
{
	switch (driveType) {
	case DRIVE_REMOVABLE: return 0x04;
	case DRIVE_FIXED:     return 0x08;
	case DRIVE_REMOTE:    return 0x10;
	case DRIVE_CDROM:     return 0x20;
	case DRIVE_RAMDISK:   return 0x40;
	default:              return 0x01;
	}
}

static bool IsSupportedVolumeType(UINT driveType)
{
	// USB flash drives are usually DRIVE_REMOVABLE; many external USB disks
	// report DRIVE_FIXED. CD/DVD media is included as another volume arrival.
	return driveType == DRIVE_REMOVABLE || driveType == DRIVE_FIXED ||
		driveType == DRIVE_CDROM;
}

static bool IsAutoPlayAllowedForVolume(int driveIndex, UINT driveType)
{
	if (driveIndex < 0 || driveIndex >= 26 || !IsSupportedVolumeType(driveType))
		return false;
	if (IsAutoPlayGloballyDisabled())
		return false;

	const DWORD disabledTypes = ReadPolicyMask(L"NoDriveTypeAutoRun");
	if ((disabledTypes & DriveTypePolicyBit(driveType)) != 0)
		return false;

	const DWORD disabledLetters = ReadPolicyMask(L"NoDriveAutoRun");
	if ((disabledLetters & (1u << driveIndex)) != 0)
		return false;
	return true;
}

static void ScheduleVolumeMask(HWND hwnd, DWORD unitMask)
{
	if (!unitMask) return;
	const DWORD wasPending = g_autoPlayMonitorState.pendingVolumes;
	const ULONGLONG now = GetTickCount64();

	for (int i = 0; i < 26; ++i) {
		const DWORD bit = (1u << i);
		if ((unitMask & bit) == 0 || (g_autoPlayMonitorState.pendingVolumes & bit) != 0)
			continue;
		const ULONGLONG last = g_autoPlayMonitorState.lastInvocation[i];
		if (last && now >= last && now - last < kAutoPlayDuplicateWindowMs)
			continue;
		g_autoPlayMonitorState.retryCount[i] = 0;
		g_autoPlayMonitorState.pendingVolumes |= bit;
	}

	if (!wasPending && g_autoPlayMonitorState.pendingVolumes &&
		!SetTimer(hwnd, kAutoPlayRetryTimer, kAutoPlayInitialDelayMs, nullptr)) {
		dbgprintf(L"AutoPlay monitor: SetTimer failed (%lu)\n", GetLastError());
		g_autoPlayMonitorState.pendingVolumes = 0;
	}
}

static void RemoveVolumeMask(HWND hwnd, DWORD unitMask)
{
	g_autoPlayMonitorState.pendingVolumes &= ~unitMask;
	for (int i = 0; i < 26; ++i) {
		if (unitMask & (1u << i))
			g_autoPlayMonitorState.retryCount[i] = 0;
	}
	if (!g_autoPlayMonitorState.pendingVolumes)
		KillTimer(hwnd, kAutoPlayRetryTimer);
}

static bool InvokeAutoPlayVerb(HWND hwnd, int driveIndex)
{
	wchar_t root[] = { (wchar_t)(L'A' + driveIndex), L':', L'\\', L'\0' };
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	info.hwnd = hwnd;
	info.lpVerb = L"autoplay";
	info.lpFile = root;
	info.nShow = SW_SHOWNORMAL;

	if (ShellExecuteExW(&info)) {
		dbgprintf(L"AutoPlay monitor: invoked shell autoplay verb for %s\n", root);
		return true;
	}
	dbgprintf(L"AutoPlay monitor: shell autoplay verb unavailable for %s (error %lu)\n",
		root, GetLastError());
	return false;
}

static void ProcessPendingVolumes(HWND hwnd)
{
	KillTimer(hwnd, kAutoPlayRetryTimer);
	const DWORD pending = g_autoPlayMonitorState.pendingVolumes;
	g_autoPlayMonitorState.pendingVolumes = 0;
	DWORD retryMask = 0;
	const ULONGLONG now = GetTickCount64();

	for (int i = 0; i < 26; ++i) {
		const DWORD bit = (1u << i);
		if ((pending & bit) == 0) continue;

		wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', L'\0' };
		const UINT driveType = GetDriveTypeW(root);
		if (driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN) {
			if (++g_autoPlayMonitorState.retryCount[i] <= kAutoPlayRetryLimit)
				retryMask |= bit;
			else
				dbgprintf(L"AutoPlay monitor: volume %s did not become ready\n", root);
			continue;
		}

		g_autoPlayMonitorState.retryCount[i] = 0;
		if (!IsAutoPlayAllowedForVolume(i, driveType)) {
			dbgprintf(L"AutoPlay monitor: skipped %s (drive type or system policy)\n", root);
			continue;
		}

		// Mark before calling into shell32; ShellExecuteEx can pump messages and
		// a duplicate DBT_DEVICEARRIVAL must not open a second prompt.
		g_autoPlayMonitorState.lastInvocation[i] = now;
		InvokeAutoPlayVerb(hwnd, i);
	}

	if (retryMask) {
		g_autoPlayMonitorState.pendingVolumes |= retryMask;
		if (!SetTimer(hwnd, kAutoPlayRetryTimer, kAutoPlayRetryDelayMs, nullptr)) {
			dbgprintf(L"AutoPlay monitor: retry timer failed (%lu)\n", GetLastError());
			g_autoPlayMonitorState.pendingVolumes = 0;
		}
	}
}

struct AutoPlayTimerContext {
	HWND hwnd;
};

static void ProcessPendingVolumesThunk(AutoPlayTimerContext* context)
{
	if (context && context->hwnd)
		ProcessPendingVolumes(context->hwnd);
}

static void HandleDeviceChange(HWND hwnd, WPARAM event, LPARAM data)
{
	if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE &&
		event != DBT_DEVICEQUERYREMOVE && event != DBT_DEVICEREMOVEPENDING)
		return;
	if (!data) return;

	const DEV_BROADCAST_HDR* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
	if (header->dbch_size < sizeof(DEV_BROADCAST_HDR) ||
		header->dbch_devicetype != DBT_DEVTYP_VOLUME ||
		header->dbch_size < sizeof(DEV_BROADCAST_VOLUME))
		return;

	const DEV_BROADCAST_VOLUME* volume = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(header);
	if (event == DBT_DEVICEARRIVAL)
		ScheduleVolumeMask(hwnd, volume->dbcv_unitmask);
	else
		RemoveVolumeMask(hwnd, volume->dbcv_unitmask);
}

LRESULT CALLBACK AutoPlayDeviceWindowProc(HWND hwnd, UINT message,
	WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_DEVICECHANGE:
		// lParam points to a broadcast structure owned by the OS. Validate it
		// under SEH because this callback runs in Explorer's process.
		__try {
			HandleDeviceChange(hwnd, wParam, lParam);
		}
		__except (Win7ExplorerRestorer::SehFilter(L"AutoPlay WM_DEVICECHANGE", GetExceptionInformation())) {
			return TRUE;
		}
		return TRUE;

	case WM_TIMER:
		if (wParam == kAutoPlayRetryTimer) {
			AutoPlayTimerContext context = { hwnd };
			if (!Win7ExplorerRestorer::SafeInvokeCtx(L"AutoPlay volume processing",
				ProcessPendingVolumesThunk, &context)) {
				g_autoPlayMonitorState.pendingVolumes = 0;
				KillTimer(hwnd, kAutoPlayRetryTimer);
			}
			return 0;
		}
		break;

	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, kAutoPlayRetryTimer);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

static DWORD WINAPI AutoPlayDeviceMonitorThreadProc(LPVOID)
{
	Win7ExplorerRestorer::ScopedCoInit com(COINIT_APARTMENTTHREADED);
	if (!com.Succeeded())
		dbgprintf(L"AutoPlay monitor: COM init failed 0x%08X\n", (unsigned)com.Result());

	HINSTANCE instance = g_hInstance ? g_hInstance : GetModuleHandleW(nullptr);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = AutoPlayDeviceWindowProc;
	wc.hInstance = instance;
	wc.lpszClassName = kAutoPlayMonitorClass;
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		dbgprintf(L"AutoPlay monitor: RegisterClassEx failed (%lu)\n", GetLastError());
		InterlockedExchange(&g_autoPlayMonitorStarted, 0);
		return 1;
	}

	// A hidden WS_POPUP is a top-level window, so it receives the basic volume
	// DBT_DEVICEARRIVAL broadcasts without registering for unrelated USB device
	// interfaces. Message-only windows do not receive these broadcasts.
	HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		kAutoPlayMonitorClass, L"", WS_POPUP, 0, 0, 0, 0,
		nullptr, nullptr, instance, nullptr);
	if (!hwnd) {
		dbgprintf(L"AutoPlay monitor: CreateWindowEx failed (%lu)\n", GetLastError());
		UnregisterClassW(kAutoPlayMonitorClass, instance);
		InterlockedExchange(&g_autoPlayMonitorStarted, 0);
		return 1;
	}

	dbgprintf(L"AutoPlay monitor: volume listener ready\n");
	MSG message = {};
	BOOL result = 0;
	while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	if (result == -1)
		dbgprintf(L"AutoPlay monitor: GetMessage failed (%lu)\n", GetLastError());
	if (IsWindow(hwnd)) DestroyWindow(hwnd);
	UnregisterClassW(kAutoPlayMonitorClass, instance);
	InterlockedExchange(&g_autoPlayMonitorStarted, 0);
	return 0;
}

} // namespace

CAutoPlayWrapper::CAutoPlayWrapper(IAutoPlayUI* autoui)
	: m_autoui(autoui), m_cRef(1)
{
}

CAutoPlayWrapper::~CAutoPlayWrapper()
{
	SafeAutoPlayRelease(m_autoui);
	m_autoui = nullptr;
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject) return E_POINTER;
	*ppvObject = nullptr;
	if (!m_autoui) return E_NOINTERFACE;

	if (riid == IID_IUnknown || riid == IID_AutoPlayUI) {
		*ppvObject = static_cast<IAutoPlayUI*>(this);
		AddRef();
		return S_OK;
	}
	return SafeAutoPlayCall(L"CAutoPlayWrapper::QueryInterface", m_autoui,
		&IAutoPlayUI::QueryInterface, riid, ppvObject);
}

ULONG STDMETHODCALLTYPE CAutoPlayWrapper::AddRef(void)
{
	return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
}

ULONG STDMETHODCALLTYPE CAutoPlayWrapper::Release(void)
{
	LONG count = InterlockedDecrement(&m_cRef);
	if (count == 0) {
		delete this;
		return 0;
	}
	return count > 0 ? static_cast<ULONG>(count) : 0;
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitVolumeAutoplay(IUnknown* p1,
	LPCWSTR p2, LPCWSTR p3, ULONG p4, ULONG p5, ULONG p6, LPCWSTR p7,
	LPCWSTR p8, int p9, LPCWSTR p10, LPCWSTR p11, HWND p12)
{
	const ULONG flags = p6 | 2;
	dbgprintf(L"CAutoPlayWrapper::InitVolumeAutoplay flags=0x%lx\n", (unsigned long)flags);
	return SafeAutoPlayCall(L"CAutoPlayWrapper::InitVolumeAutoplay", m_autoui,
		&IAutoPlayUI::InitVolumeAutoplay, p1, p2, p3, p4, p5, flags,
		p7, p8, p9, p10, p11, p12);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitNoContentAutoplay(IUnknown* p1,
	REFGUID p2, LPCWSTR p3, ULONG p4, int p5, LPCWSTR p6, LPCWSTR p7, LPCWSTR p8)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::InitNoContentAutoplay", m_autoui,
		&IAutoPlayUI::InitNoContentAutoplay, p1, p2, p3, p4, p5, p6, p7, p8);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::InitDirectAutoPlay(IUnknown* p1,
	LPCWSTR p2, HWND p3)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::InitDirectAutoPlay", m_autoui,
		&IAutoPlayUI::InitDirectAutoPlay, p1, p2, p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::ToastPromptForChkDsk(LPCWSTR p1,
	int* p2, int* p3)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::ToastPromptForChkDsk", m_autoui,
		&IAutoPlayUI::ToastPromptForChkDsk, p1, p2, p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::LaunchDeviceHandler(LPCWSTR p1,
	LPCWSTR p2, LPCWSTR p3)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::LaunchDeviceHandler", m_autoui,
		&IAutoPlayUI::LaunchDeviceHandler, p1, p2, p3);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::IsDialogClosed(void)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::IsDialogClosed", m_autoui,
		&IAutoPlayUI::IsDialogClosed);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::SniffComplete(ULONG p1)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::SniffComplete", m_autoui,
		&IAutoPlayUI::SniffComplete, p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::CloseDialog(void)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::CloseDialog", m_autoui,
		&IAutoPlayUI::CloseDialog);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::AddContentType(ULONG p1)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::AddContentType", m_autoui,
		&IAutoPlayUI::AddContentType, p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::MoreInterfaceArrived(LPCWSTR p1)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::MoreInterfaceArrived", m_autoui,
		&IAutoPlayUI::MoreInterfaceArrived, p1);
}

HRESULT STDMETHODCALLTYPE CAutoPlayWrapper::SetChkDskCompleted(void)
{
	return SafeAutoPlayCall(L"CAutoPlayWrapper::SetChkDskCompleted", m_autoui,
		&IAutoPlayUI::SetChkDskCompleted);
}

// Repairs only user policies that completely suppress volume AutoPlay. Values
// missing from HKCU are untouched, HKLM is never written, and FixAutoPlay=0
// lets the user opt out. Calls from InstallShellFixes are also SEH-guarded.
void EnsureAutoPlayDefaults()
{
	if (ReadAdvancedDwordLocal(L"FixAutoPlay", 1) == 0) {
		dbgprintf(L"EnsureAutoPlayDefaults: disabled (FixAutoPlay=0)\n");
		return;
	}

	Win7ExplorerRestorer::ScopedRegKey policies;
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER,
		kPoliciesExplorerKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, policies.Put());
	if (status != ERROR_SUCCESS) {
		dbgprintf(L"EnsureAutoPlayDefaults: no per-user Policies\\Explorer key, nothing to repair\n");
		return; // Do not create a policy key just to edit a missing value.
	}

	DWORD value = 0, bytes = sizeof(value), type = 0;
	if (RegQueryValueExW(policies.Get(), L"NoDriveTypeAutoRun", nullptr,
		&type, (BYTE*)&value, &bytes) == ERROR_SUCCESS &&
		type == REG_DWORD && value == 0xFF) {
		value = 0x91; // common Windows default: unknown + remote + reserved disabled
		if (RegSetValueExW(policies.Get(), L"NoDriveTypeAutoRun", 0,
			REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS)
			dbgprintf(L"EnsureAutoPlayDefaults: NoDriveTypeAutoRun 0xFF -> 0x91\n");
	}

	value = 0;
	bytes = sizeof(value);
	type = 0;
	if (RegQueryValueExW(policies.Get(), L"NoAutoplayfornonVolume", nullptr,
		&type, (BYTE*)&value, &bytes) == ERROR_SUCCESS &&
		type == REG_DWORD && value != 0) {
		value = 0;
		if (RegSetValueExW(policies.Get(), L"NoAutoplayfornonVolume", 0,
			REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS)
			dbgprintf(L"EnsureAutoPlayDefaults: NoAutoplayfornonVolume -> 0\n");
	}
}

void StartAutoPlayDeviceMonitor()
{
	if (ReadAdvancedDwordLocal(L"AutoPlayDeviceNotifications", 1) == 0) {
		dbgprintf(L"AutoPlay monitor: disabled (AutoPlayDeviceNotifications=0)\n");
		return;
	}
	if (InterlockedCompareExchange(&g_autoPlayMonitorStarted, 1, 0) != 0)
		return;

	Win7ExplorerRestorer::ScopedHandle thread(CreateThread(nullptr, 0,
		AutoPlayDeviceMonitorThreadProc, nullptr, 0, nullptr));
	if (!thread.Get()) {
		InterlockedExchange(&g_autoPlayMonitorStarted, 0);
		dbgprintf(L"AutoPlay monitor: CreateThread failed (%lu)\n", GetLastError());
		return;
	}
	// Closing the creator's handle does not stop the listener thread. Its
	// lifetime is intentionally bounded by the private Explorer process.
}
