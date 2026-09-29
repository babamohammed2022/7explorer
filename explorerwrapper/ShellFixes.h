#pragma once
// 7explorer fork: fixes for Win7 shell targets that no longer exist on
// Windows 10/11, UWP start-up protection and DWM transparency prerequisites.
#include "common.h"

namespace ex7 {

// Installs the ShellExecuteEx/ShellExecute remapping on explorer's IAT and
// applies the registry prerequisites. Every step is SEH-guarded.
void InstallShellFixes(HMODULE hSelf);

// Crash sentinel around the immersive (UWP) start-up.
// Returns false when UWP must stay off for this session.
bool ImmersiveStartupAllowed();
void ImmersiveStartupBegin();
void ImmersiveStartupSucceeded();

// CreateTwinUI_UWP() under SEH + sentinel bookkeeping.
void SafeCreateTwinUI_UWP();

// Called after the system CLSID_SysTray object is created.
void OnSystemSysTrayCreated();
void OnSysTrayCreateBegin();

} // namespace ex7
