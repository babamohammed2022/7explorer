#pragma once
// 7explorer fork: plain Win32 (non-immersive) context menus for the tray
// DLLs SndVolSSO.dll and pnidui.dll. Same technique as ExplorerPatcher
// (valinet/ExplorerPatcher, dllmain.c HookImmersiveMenuFunctions, GPL-2.0
// project; credits to valinet and contributors): find
// ImmersiveContextMenuHelper::ApplyOwnerDrawToMenu by its x64 prologue in
// the module's .text and make it a no-op returning S_OK, so the menu is
// shown by TrackPopupMenu as a classic themed Win32 menu.
// Opt-out: HKCU\...\Explorer\Advanced\ClassicTrayMenus = 0.
#include "common.h"
namespace Win7ExplorerRestorer {
void ClassicMenusFor(HMODULE module, const wchar_t* name); // idempotent, SEH inside
}
