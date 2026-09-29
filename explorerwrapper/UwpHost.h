#pragma once
// 7explorer fork: UWP/"metro host" delegation shim.
// Since Windows 8 the modern explorer.exe is also the host of the immersive
// shell services (view management used by ApplicationFrameHost/CoreWindow
// apps such as Settings). 7explorer tries to start that stack in-process
// (TwinUI, ImmersiveShell.cpp); when it is not running, or an activation
// produces no app window, the native stand-alone host that Windows 10 1809+
// and 11 ship, %SystemRoot%\System32\ShellAppRuntime.exe, is started instead
// (winclassic.net explorer7 thread, Ingan121: "Running ShellAppRuntime.exe
// ... allows running UWP/Immersive stuff without running explorer.exe").
// Nothing is replaced on disk; the host runs in a kill-on-close job so it
// ends with this shell. Explorer\Advanced UwpHostRuntime: 0 = never,
// 1 = automatic (default), 2 = always at start-up, 3 = always, before the
// Win7 desktop. UwpEarlyHost: 1 = start early (set by itself when a late host
// did not help), 0 = never early.
#include "common.h"
namespace ex7 { namespace uwp {
void SetTwinUiStarted(bool ok);        // ImmersiveShell.cpp result
void StartupCheck();
void EarlyStart();                      // UwpHostRuntime=3 or UwpEarlyHost=1 (set automatically)                    // ~10 s after the tray is up
int CountAppWindows();                  // visible UWP frame/core windows
// After a UWP launch that reported success: if no app window appears within
// a few seconds, start the host and repeat the launch once.
// kind 0: uri via ShellExecute, kind 1: AUMID via IApplicationActivationManager.
void WatchActivation(int kind, const wchar_t* target, const wchar_t* args, int windowsBefore);
bool EnsureHost(const wchar_t* why);
}}
