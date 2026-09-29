#include "RegistryManager.h"
#include "dbgprint.h"

void CRegistryManager::_OpenKeys()
{
	if (!m_hKeyMachine)
		RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			c_szSubkey,
			NULL,
			KEY_READ | KEY_WRITE,
			&m_hKeyMachine
		);

	if (!m_hKeyUser)
		RegOpenKeyExW(
			HKEY_CURRENT_USER,
			c_szSubkey,
			NULL,
			KEY_READ | KEY_WRITE,
			&m_hKeyUser
		);
}

CRegistryManager::CRegistryManager()
	: m_hKeyMachine(NULL)
	, m_hKeyUser(NULL)
{
}

// NOTE: no destructor by design (see RegistryManager.h): with /ENTRY:DllMain
// there is no CRT termination pass, so a dtor could never run; worse, the
// atexit() references it emits break the static-CRT link (LNK2001
// __vcrt_initialize et al). The lazily opened keys are process-lifetime
// handles reclaimed by the OS at exit.

LSTATUS CRegistryManager::QueryValue(LPCWSTR lpValueName, LPBYTE lpData, DWORD cbData, LPDWORD lpType)
{
	_OpenKeys();
	LSTATUS res = RegQueryValueExW(m_hKeyUser, lpValueName, NULL, lpType, lpData, &cbData);
	if (res != ERROR_SUCCESS)
	{
		return RegQueryValueExW(m_hKeyMachine, lpValueName, NULL, lpType, lpData, &cbData);
	}
	return res;
}

LSTATUS CRegistryManager::SetValue(LPCWSTR lpValueName, DWORD dwType, LPCBYTE lpData, DWORD cbData)
{
	_OpenKeys();
	return RegSetValueExW(m_hKeyUser, lpValueName, NULL, dwType, lpData, cbData);
}
