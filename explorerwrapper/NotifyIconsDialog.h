#pragma once
// 7explorer fork: built-in "Notification Area Icons" window.
// Windows 11 24H2 removed the Control Panel page {05D7B0F4-...} that the Win7
// "Customize..." links open. The Win7 explorer still implements the private
// ITrayNotify interface (CLSID_TrayNotify, registered by explorer itself), so
// the page is recreated here: list of icons, behaviour per icon, and "Always
// show all icons". Runs on its own STA thread; SEH-guarded.
#include "common.h"
namespace ex7 {
// true if the window was started (or already open and brought to front).
bool ShowNotifyIconsDialog();
}
