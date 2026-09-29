#include "UwpHost.h"
#include "SafeGuards.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

namespace ex7 {
void LogText(const wchar_t* text);
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def);
namespace uwp {
namespace {

void Log(const wchar_t* fmt, ...)
{
	wchar_t msg[600], line[660];
	va_list ap; va_start(ap, fmt);
	wvnsprintfW(msg, ARRAYSIZE(msg), fmt, ap);
	va_end(ap);
	wnsprintfW(line, ARRAYSIZE(line), L"[ex7][uwp-host] %s", msg);
	LogText(line);
}

volatile LONG g_twinOk = 0;
volatile LONG g_hostStarted = 0;
HANDLE g_job = nullptr; // intentionally kept open: closing it ends the host

DWORD Mode() { return ReadAdvancedDwordPublic(L"UwpHostRuntime", 1); }

bool HostRunning()
{
	ScopedHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (snap.Get() == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = { sizeof(pe) };
	for (BOOL ok = Process32FirstW(snap.Get(), &pe); ok; ok = Process32NextW(snap.Get(), &pe))
		if (!lstrcmpiW(pe.szExeFile, L"ShellAppRuntime.exe")) return true;
	return false;
}

BOOL CALLBACK CountProc(HWND h, LPARAM l)
{
	wchar_t cls[64];
	if (IsWindowVisible(h) && GetClassNameW(h, cls, 64) &&
		(!lstrcmpW(cls, L"ApplicationFrameWindow") || !lstrcmpW(cls, L"Windows.UI.Core.CoreWindow")))
		++*(int*)l;
	return TRUE;
}

const CLSID kCLSID_AAM = { 0x45BA127D, 0x10A8, 0x46EA, { 0x8A, 0xB7, 0x56, 0xEA, 0x90, 0x78, 0x94, 0x3C } };

HRESULT ActivateAumid(const wchar_t* aumid, const wchar_t* args)
{
	HRESULT hi = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	HRESULT hr;
	DWORD pid = 0;
	{
		ComPtr<IApplicationActivationManager> aam;
		hr = CoCreateInstance(kCLSID_AAM, nullptr, CLSCTX_LOCAL_SERVER, __uuidof(IApplicationActivationManager), aam.PutVoid());
		if (SUCCEEDED(hr)) {
			CoAllowSetForegroundWindow(aam.Get(), nullptr);
			hr = aam->ActivateApplication(aumid, args && *args ? args : nullptr, AO_NONE, &pid);
		}
	}
	if (SUCCEEDED(hi)) CoUninitialize();
	Log(L"retry ActivateApplication(%s) hr=0x%08X pid=%u", aumid, (DWORD)hr, pid);
	return hr;
}

struct Watch { int kind; int before; wchar_t target[512]; wchar_t args[512]; };

void WatchUnsafe(Watch* w)
{
	for (int i = 0; i < 28; ++i) { // ~7 s
		Sleep(250);
		if (CountAppWindows() > w->before) return; // the app came up
	}
	if (InterlockedCompareExchange(&g_hostStarted, 0, 0) || HostRunning()) {
		Log(L"no window for %s, host already running: nothing more to try", w->target);
		return;
	}
	Log(L"no app window 7 s after launching %s", w->target);
	if (!EnsureHost(L"activation produced no window")) return;
	Sleep(2500); // let the host register its services
	if (w->kind == 1) ActivateAumid(w->target, w->args);
	else {
		HINSTANCE r = ShellExecuteW(nullptr, nullptr, w->target, nullptr, nullptr, SW_SHOWNORMAL);
		Log(L"retry %s -> %d", w->target, (int)(INT_PTR)r);
	}
}

DWORD WINAPI WatchThread(LPVOID p)
{
	Watch* w = (Watch*)p;
	SafeInvokeCtx<Watch>(L"uwp watch", WatchUnsafe, w);
	delete w;
	return 0;
}

} // namespace

void SetTwinUiStarted(bool ok)
{
	InterlockedExchange(&g_twinOk, ok ? 1 : 0);
	Log(L"in-process TwinUI started=%d", ok ? 1 : 0);
}

int CountAppWindows()
{
	int n = 0;
	EnumWindows(CountProc, (LPARAM)&n);
	return n;
}

bool EnsureHost(const wchar_t* why)
{
	if (Mode() == 0) return false;
	if (HostRunning()) { InterlockedExchange(&g_hostStarted, 1); return true; }
	if (InterlockedExchange(&g_hostStarted, 1)) return true;
	wchar_t exe[MAX_PATH];
	if (!GetSystemDirectoryW(exe, MAX_PATH) || !PathAppendW(exe, L"ShellAppRuntime.exe") ||
		GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
		Log(L"ShellAppRuntime.exe not present on this build (%s): no UWP host", why);
		return false; // e.g. Windows 8.1 / old Windows 10
	}
	if (!g_job) {
		g_job = CreateJobObjectW(nullptr, nullptr);
		if (g_job) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK |
				JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK; // children (apps) are not tied to the job
			SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
		}
	}
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	wchar_t cmd[MAX_PATH + 4];
	wnsprintfW(cmd, ARRAYSIZE(cmd), L"\"%s\"", exe);
	BOOL ok = CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
	if (!ok) { Log(L"CreateProcess %s failed (%u)", exe, GetLastError()); InterlockedExchange(&g_hostStarted, 0); return false; }
	ScopedHandle th(pi.hThread), ph(pi.hProcess);
	BOOL inJob = g_job ? AssignProcessToJobObject(g_job, pi.hProcess) : FALSE;
	ResumeThread(pi.hThread);
	Log(L"started ShellAppRuntime.exe pid=%u (%s), job=%d", pi.dwProcessId, why, inJob);
	return true;
}

void StartupCheck()
{
	__try {
		DWORD m = Mode();
		if (m == 2) EnsureHost(L"UwpHostRuntime=2");
		else if (m == 1 && !InterlockedCompareExchange(&g_twinOk, 0, 0)) EnsureHost(L"in-process TwinUI not running");
	}
	__except (SehFilter(L"uwp StartupCheck", GetExceptionInformation())) {}
}

void WatchActivation(int kind, const wchar_t* target, const wchar_t* args, int windowsBefore)
{
	if (Mode() == 0 || !target || !*target) return;
	if (InterlockedCompareExchange(&g_hostStarted, 0, 0)) return;
	Watch* w = new Watch();
	if (!w) return;
	w->kind = kind; w->before = windowsBefore;
	lstrcpynW(w->target, target, ARRAYSIZE(w->target));
	lstrcpynW(w->args, args ? args : L"", ARRAYSIZE(w->args));
	HANDLE t = CreateThread(nullptr, 0, WatchThread, w, 0, nullptr);
	if (t) CloseHandle(t); else delete w;
}

}} // namespace ex7::uwp
