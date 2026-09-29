#pragma once
// 7explorer fork: Win32 (non-immersive) menus for the tray DLLs and hardcoded
// actions for the volume menu.
// - Owner-draw stripping: same idea as the Windhawk mod "Non-Immersive Taskbar
//   Context Menu Lite" by Anixx (https://github.com/Anixx): SetMenuItemInfoW /
//   InsertMenuItemW calls that add MFT_OWNERDRAW are passed on without it, so
//   the immersive helper cannot take over drawing. Here it is limited to calls
//   coming from SndVolSSO.dll, pnidui.dll and stobject.dll.
// - Volume menu: the chosen command is read with TPM_RETURNCMD; "volume mixer"
//   opens SndVol.exe and "sound settings" opens mmsys.cpl directly (the
//   Settings pages they target do not open under the Win7 shell).
// Opt-out: Explorer\Advanced ClassicTrayMenus=0 / VolumeMenuActions=0.
#include "common.h"
namespace ex7 {
void InstallTrayMenus();
void InstallControlPanelOpenHook();
bool OpenNotifyIconsPage(); // explorer.exe shell:::{05D7B0F4-...}, SEH-guarded // IOpenControlPanel::Open("...NotificationAreaIcons")
}
