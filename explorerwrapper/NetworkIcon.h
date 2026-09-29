#pragma once
// 7explorer fork: network tray icon on builds without pnidui.dll
// (Windows 11 24H2+). Same approach as ExplorerPatcher: pnidui.dll
// 10.0.22621.3810 from the Microsoft symbol server (SHA-256 pinned), its
// .mui files from ExplorerPatcher's repository at a pinned commit (SHA-256
// pinned), cached under %LOCALAPPDATA%\7explorer\pnidui. The system
// stobject starts it as the network shell service object
// {C2796011-81BA-4148-8FCA-C6643245113F} after we re-point its unused
// Windows To Go SSO slot, and its CoCreateInstance is routed to the cache.
#include "common.h"

namespace ex7 {
namespace net {

// DllMain (explorer only): starts a worker thread that fills the cache.
void StartBackgroundPrepare();

// Called whenever stobject.dll is (or may be) loaded. Idempotent.
void OnStobjectLoaded(HMODULE stobject);

// Path of the cached pnidui.dll (false if not downloaded yet).
bool CachedDllPath(wchar_t* out);
bool NetworkIconWanted();
bool NetworkSsoCreated(); // pnidui SSO object running (real icon)

// Fallback (NetworkTrayIcon.cpp): if pnidui is still not running ~15 s after
// the tray started, show our own notification icon (connectivity from the
// Network List Manager, glyphs from the cached pnidui.dll resources).
void StartFallbackTrayIcon();

} // namespace net
namespace w81 {
bool Sha256OfFile(const wchar_t* path, char hex[65]);
}
} // namespace ex7
