#pragma once
// 7explorer fork: Aero (thick, DWM-drawn) borders for the legacy clock,
// battery and Action Center flyouts.
//
// Based on "Aero Flyout Fix" by aubymori (https://github.com/aubymori),
// Windhawk mod aero-flyout-fix 1.1.0 (https://windhawk.net/mods/aero-flyout-fix).
// The original hooks private symbols (CTrayClock::s_WndProc, UpdateFlyoutUI,
// CHCFlyoutWindow::...::s_WndProc); the wrapper has no symbol engine at run
// time, so the same effect is obtained from exported APIs only:
//  * CreateWindowExW / CreateWindowInBand: add WS_THICKFRAME + WS_EX_TOOLWINDOW
//    to ClockFlyoutWindow, BatMeterFlyout and WHCFlyoutWindow (as the mod does);
//  * a window subclass turns the sizing hit-test codes into HTBORDER (the
//    mod's s_WndProc hooks: frame visible, window not resizable) and keeps
//    WS_THICKFRAME when the flyout code rewrites its style (the mod's
//    UpdateFlyoutUI hook).
// Opt-out: HKCU\...\Explorer\Advanced\AeroFlyoutFrames = 0.
#include "common.h"

namespace ex7 {
void InstallFlyoutFrames();
}
