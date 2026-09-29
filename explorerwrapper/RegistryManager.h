#pragma once
#include "common.h"
#include <Shlwapi.h>

class CRegistryManager
{
private:
	HKEY m_hKeyMachine;
	HKEY m_hKeyUser;
	void _OpenKeys();

public:
	CRegistryManager();
	// NOTE: intentionally NO destructor. This DLL links with /ENTRY:DllMain
	// (no CRT startup/termination), so a non-trivial dtor would only emit
	// atexit() references that break the static-CRT link (LNK2001) and could
	// never run anyway. The two lazily opened keys below are process-lifetime
	// handles by design (opened once each, reclaimed by the OS at exit).

	LSTATUS QueryValue(LPCWSTR lpValueName, LPBYTE lpData, DWORD cbData, LPDWORD lpType = nullptr);
	HRESULT QueryValueWithFallback(LPCWSTR lpValueName, LPBYTE lpData, DWORD cbData, LPDWORD lpType = nullptr, DWORD dwDefault = 0);
	LSTATUS SetValue(LPCWSTR lpValueName, DWORD dwType, LPCBYTE lpData, DWORD cbData);
};

static CRegistryManager g_registry;

const LPWSTR c_szSubkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
