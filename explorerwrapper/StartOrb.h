#pragma once
// 7explorer fork: Start button ("orb") image selection.
//
// Lets the user pick the taskbar Start button image from:
//   1) OrbFile      - a custom .bmp (absolute path, or relative to explorer.exe's directory)
//   2) OrbDirectory - a preset folder under <exedir>\orbs\<name>\ (upstream behaviour)
//   3) built-in     - the image embedded in explorer.exe (always the final fallback)
//
// Credits / inspiration: Open-Shell (https://github.com/Open-Shell/Open-Shell-Menu,
// MIT License, (c) the Open-Shell and Classic Shell authors) popularised replacing
// the Start button with a user-supplied image and a bundled set of presets. No
// Open-Shell source is copied here; only the idea. Keep this notice if you reuse
// the approach. The preset directory mechanism comes from explorer7 upstream.
//
// Safety model (same rules as SafeGuards.h):
//   - RAII for the GDI bitmap so no path leaks it on early return.
//   - Everything that touches user-controlled input runs under SEH via
//     ex7::SafeInvokeCtx; any failure falls back to the original LoadImageW.
//   - The bitmap is validated (type/size) before it is handed to explorer.
#include "common.h"
#include "dbgprint.h"
#include "SafeGuards.h"
#include "RegistryManager.h"
#include <strsafe.h>
#include <shlwapi.h>

namespace ex7 {

// Upper bound for a sane Start button strip; rejects corrupt/hostile files.
static const LONG kMaxOrbDim = 1024;

// RAII owner for an HGDIOBJ (HBITMAP). Release() hands ownership to the caller.
class ScopedGdiObject {
public:
	explicit ScopedGdiObject(HGDIOBJ h = nullptr) : m_h(h) {}
	~ScopedGdiObject() { Reset(); }
	HGDIOBJ Get() const { return m_h; }
	HGDIOBJ Release() { HGDIOBJ h = m_h; m_h = nullptr; return h; }
	void Reset(HGDIOBJ h = nullptr) { if (m_h) DeleteObject(m_h); m_h = h; }
private:
	HGDIOBJ m_h;
	ScopedGdiObject(const ScopedGdiObject&) = delete;
	ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;
};

// Reads a REG_SZ value from Explorer\Advanced (HKCU, then HKLM). Always
// NUL-terminated on success; returns false if missing, empty or not REG_SZ.
inline bool ReadOrbRegString(const WCHAR* name, WCHAR* buf, DWORD cch)
{
	if (!buf || cch < 2) return false;
	buf[0] = L'\0';
	DWORD type = 0;
	LSTATUS res = g_registry.QueryValue(name, (LPBYTE)buf, (cch - 1) * sizeof(WCHAR), &type);
	buf[cch - 1] = L'\0'; // guarantees termination even for unterminated data
	return res == ERROR_SUCCESS && type == REG_SZ && buf[0] != L'\0';
}

inline bool OrbIsRegularFile(const WCHAR* path)
{
	DWORD a = GetFileAttributesW(path);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Only local, .bmp paths are accepted: a UNC/network path could stall explorer's
// start-up, and the loader below only understands bitmaps.
inline bool OrbPathAllowed(const WCHAR* path)
{
	if (!path || !*path) return false;
	if (path[0] == L'\\' && path[1] == L'\\') return false; // UNC and \\?\ forms
	return lstrcmpiW(PathFindExtensionW(path), L".bmp") == 0;
}

inline bool OrbIsAbsolute(const WCHAR* p)
{
	return (((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'));
}

// Preset names must stay inside <exedir>\orbs\ : no drive, no rooted path, no "..".
inline bool OrbPresetNameSafe(const WCHAR* s)
{
	if (!s || !*s) return false;
	if (s[0] == L'\\' || s[0] == L'/') return false;
	for (const WCHAR* p = s; *p; ++p)
		if (*p == L':') return false;
	return StrStrW(s, L"..") == nullptr;
}

// Picks the bitmap file to try. Returns false when nothing is configured or the
// configured files do not exist (caller then uses the built-in image).
// getPresetFileName is only invoked if the preset path is actually needed.
inline bool ResolveOrbPath(WCHAR* out /* >= MAX_PATH */, void (*getPresetFileName)(LPWSTR))
{
	WCHAR exeDir[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return false;
	if (!PathRemoveFileSpecW(exeDir)) return false;

	// 1) Custom file
	WCHAR custom[MAX_PATH];
	if (ReadOrbRegString(L"OrbFile", custom, MAX_PATH))
	{
		WCHAR full[MAX_PATH];
		bool ok;
		if (OrbIsAbsolute(custom))
			ok = SUCCEEDED(StringCchCopyW(full, MAX_PATH, custom));
		else
			ok = PathCombineW(full, exeDir, custom) != nullptr;

		if (ok && OrbPathAllowed(full) && OrbIsRegularFile(full))
		{
			StringCchCopyW(out, MAX_PATH, full);
			return true;
		}
		dbgprintf(L"[ex7] OrbFile '%s' not usable, trying preset", custom);
	}

	// 2) Preset directory
	WCHAR preset[MAX_PATH];
	if (getPresetFileName && ReadOrbRegString(L"OrbDirectory", preset, MAX_PATH))
	{
		if (!OrbPresetNameSafe(preset))
		{
			dbgprintf(L"[ex7] OrbDirectory '%s' rejected", preset);
			return false;
		}
		WCHAR file[MAX_PATH];
		file[0] = L'\0';
		getPresetFileName(file);
		file[MAX_PATH - 1] = L'\0';
		if (!file[0]) return false;

		WCHAR full[MAX_PATH * 3];
		if (FAILED(StringCchPrintfW(full, ARRAYSIZE(full), L"%s\\orbs\\%s\\%s.bmp", exeDir, preset, file)))
			return false;
		if (lstrlenW(full) >= MAX_PATH) return false;
		if (OrbIsRegularFile(full))
		{
			StringCchCopyW(out, MAX_PATH, full);
			return true;
		}
	}
	return false;
}

// Loads and validates the bitmap. Ownership of the HBITMAP goes to the caller.
inline HBITMAP LoadOrbBitmapFromFile(const WCHAR* path, UINT fuLoad)
{
	ScopedGdiObject bmp(LoadImageW(nullptr, path, IMAGE_BITMAP, 0, 0, fuLoad | LR_LOADFROMFILE));
	if (!bmp.Get()) return nullptr;

	BITMAP bm;
	ZeroMemory(&bm, sizeof(bm));
	if (GetObjectW(bmp.Get(), sizeof(bm), &bm) != (int)sizeof(bm)) return nullptr;
	if (bm.bmWidth < 1 || bm.bmHeight < 1 || bm.bmWidth > kMaxOrbDim || bm.bmHeight > kMaxOrbDim)
	{
		dbgprintf(L"[ex7] orb bitmap %ldx%ld rejected", bm.bmWidth, bm.bmHeight);
		return nullptr;
	}
	return (HBITMAP)bmp.Release();
}

// SEH-guarded unit of work. Plain struct + plain function: the __try lives in
// SafeInvokeCtx, which owns no C++ objects (rule C2712).
struct OrbRequest {
	UINT fuLoad;
	void (*getPresetFileName)(LPWSTR);
	HBITMAP result;
};

inline void OrbWork(OrbRequest* r)
{
	WCHAR path[MAX_PATH];
	if (ResolveOrbPath(path, r->getPresetFileName))
		r->result = LoadOrbBitmapFromFile(path, r->fuLoad);
}

} // namespace ex7
