#pragma once
// 7explorer fork: built-in "Notification Area Icons" window.
// The Win7 "Customize..." links open the system Control Panel page
// {05D7B0F4-...}: that page STILL EXISTS on Windows 11 24H2/25H2 (its CLSID
// is registered and the window opens - verified in test31), but on 24H2 it
// renders EMPTY, so this in-process recreation is the usable alternative.
// It offers: list of icons, behaviour per icon, and "Always show all icons".
// When to use it is decided by NotifyIconsUseSettings (see ShellFixes.cpp:
// 0 = auto -> system page when registered, built-in otherwise; 3 = always
// built-in, recommended on 24H2). Runs on its own STA thread; SEH-guarded.
#include "common.h"
namespace Win7ExplorerRestorer {
// true if the window was started (or already open and brought to front).
bool ShowNotifyIconsDialog();
}
