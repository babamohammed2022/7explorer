#pragma once
// 7explorer fork: Windows 8.1 battery flyout (stobject.dll + batmeter.dll
// 6.3.9600.x) downloaded on the user's machine from the Microsoft symbol
// server, verified, cached under %LOCALAPPDATA%\7explorer and used as the
// SysTray shell service object (CLSID_SysTray) instead of the system one.
// See LegacyBatteryFlyout.cpp for the full design notes.
#include "common.h"

namespace ex7 {
namespace w81 {

// Called once from DllMain (explorer only). Never blocks: starts a worker
// thread that downloads/verifies/prepares the cache if needed.
void StartBackgroundPrepare();

// Called from the CoCreateInstance hook for CLSID_SysTray on the shell
// thread. Uses the cache only (no network). Returns true and fills *phr /
// *ppv when the 8.1 object was created; false = use the system stobject.
bool TryCreateSysTray(LPUNKNOWN pUnkOuter, REFIID riid, void** ppv, HRESULT* phr);

// W81SysTrayWrapper (default 1): wrap the 8.1 object in CSysTrayWrapper,
// which translates the Win7 shell-service-object commands to the Win8 ones.
bool WrapSysTray();

// True once the 8.1 SysTray object is running in this process.
bool IsActive();

} // namespace w81
} // namespace ex7
