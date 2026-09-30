#pragma once
// 7explorer fork: Start button ("orb") image selection (BMP and PNG).
//
// Lets the user pick the taskbar Start button image from:
//   1) OrbFile      - a custom .bmp or .png (absolute path, or relative to explorer.exe's directory)
//   2) OrbDirectory - a preset folder under <exedir>\orbs\<name>\ (upstream behaviour)
//   3) built-in     - the image embedded in explorer.exe (always the final fallback)
//
// Credits / inspiration: Open-Shell (https://github.com/Open-Shell/Open-Shell-Menu,
// MIT License, (c) the Open-Shell and Classic Shell authors) popularised replacing
// the Start button with a user-supplied image (supporting 32-bit PNG with alpha
// transparency) and a bundled set of presets. No Open-Shell source is copied here;
// only the idea and behavioral compatibility. Keep this notice if you reuse
// the approach. The preset directory mechanism comes from explorer7 upstream.
//
// Safety model (same rules as SafeGuards.h):
//   - RAII for GDI bitmap, COM interfaces (ComPtr) and CoInitialize (ScopedCoInit).
//   - Everything that touches user-controlled input runs under SEH via
//     Win7ExplorerRestorer::SafeInvokeCtx; any failure falls back to the original LoadImageW.
//   - Functions executed under SEH use plain structs and own no C++ objects with
//     destructors (compiler rule C2712).
//   - Bitmaps are validated (type/size/bounds) before they are handed to explorer.
#include "common.h"
#include "dbgprint.h"
#include "SafeGuards.h"
#include "RegistryManager.h"
#include <wincodec.h>
#include <shlwapi.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace Win7ExplorerRestorer {

// Upper bound for a sane Start button strip; rejects corrupt/hostile files.
static const LONG kMaxOrbDim = 1024;
// Maximum allowed custom file size (4 MB) to prevent pathological memory usage.
static const DWORD kMaxOrbFileSize = 4 * 1024 * 1024;

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

// RAII owner for device contexts (compatible DCs and window DCs).
class ScopedDC {
public:
	explicit ScopedDC(HDC hdc = nullptr, HWND hwnd = nullptr, bool isCompatible = false)
		: m_hdc(hdc), m_hwnd(hwnd), m_isCompatible(isCompatible) {}
	ScopedDC(ScopedDC&& other) noexcept
		: m_hdc(other.m_hdc), m_hwnd(other.m_hwnd), m_isCompatible(other.m_isCompatible) {
		other.m_hdc = nullptr;
	}
	ScopedDC& operator=(ScopedDC&& other) noexcept {
		if (this != &other) {
			Reset();
			m_hdc = other.m_hdc;
			m_hwnd = other.m_hwnd;
			m_isCompatible = other.m_isCompatible;
			other.m_hdc = nullptr;
		}
		return *this;
	}
	~ScopedDC() { Reset(); }
	HDC Get() const { return m_hdc; }
	void Reset(HDC hdc = nullptr, HWND hwnd = nullptr, bool isCompatible = false) {
		if (m_hdc) {
			if (m_isCompatible) DeleteDC(m_hdc);
			else ReleaseDC(m_hwnd, m_hdc);
			m_hdc = nullptr;
		}
		m_hdc = hdc;
		m_hwnd = hwnd;
		m_isCompatible = isCompatible;
	}
private:
	HDC m_hdc;
	HWND m_hwnd;
	bool m_isCompatible;
	ScopedDC(const ScopedDC&) = delete;
	ScopedDC& operator=(const ScopedDC&) = delete;
};

// Structure representing the 3 standard button states (idle/normal, hover/hot, pressed)
// inspired by Open-Shell / Classic Shell start button architecture.
enum StartOrbState {
	ORB_STATE_NORMAL  = 0, // Idle / normal state
	ORB_STATE_HOT     = 1, // Hover / mouse over state
	ORB_STATE_PRESSED = 2, // Pressed / Start menu open state
	ORB_STATE_COUNT   = 3
};

struct StartOrbMetrics {
	LONG frameWidth;
	LONG frameHeight;
	LONG totalHeight;
	bool isThreeState;
};

// Calculates frame metrics for the start orb strip (idle, hover, pressed).
// In Open-Shell / Windows 7 Explorer, orbs are structured vertically with 3 frames:
// Frame 0: Idle/Normal, Frame 1: Hover/Hot, Frame 2: Pressed.
inline bool CalculateOrbMetrics(LONG width, LONG height, StartOrbMetrics* outMetrics)
{
	if (!outMetrics || width < 1 || height < 1)
		return false;

	if (height >= width * 3)
	{
		// 3-state vertical strip (Normal, Hot, Pressed)
		outMetrics->frameWidth = width;
		outMetrics->frameHeight = height / 3;
		outMetrics->totalHeight = height;
		outMetrics->isThreeState = true;
	}
	else if (width >= height * 3)
	{
		// Horizontal 3-state strip
		outMetrics->frameWidth = width / 3;
		outMetrics->frameHeight = height;
		outMetrics->totalHeight = height * 3;
		outMetrics->isThreeState = true;
	}
	else
	{
		// Single frame: replicate into 3 states (Normal = Hot = Pressed)
		outMetrics->frameWidth = width;
		outMetrics->frameHeight = height;
		outMetrics->totalHeight = height * 3;
		outMetrics->isThreeState = false;
	}
	return true;
}

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
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
		return false;
	if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return false;
	if (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > kMaxOrbFileSize)
	{
		dbgprintf(L"[Win7ExplorerRestorer] Orb file '%s' exceeds size limit", path);
		return false;
	}
	return true;
}

// Accepts only local .bmp and .png files. UNC and network paths could stall explorer's
// start-up and are rejected.
inline bool OrbPathAllowed(const WCHAR* path)
{
	if (!path || !*path) return false;
	if (path[0] == L'\\' && path[1] == L'\\' ) return false; // UNC and \\?\ forms
	const WCHAR* ext = PathFindExtensionW(path);
	if (!ext) return false;
	return (lstrcmpiW(ext, L".bmp") == 0 || lstrcmpiW(ext, L".png") == 0);
}

inline bool OrbIsPng(const WCHAR* path)
{
	const WCHAR* ext = PathFindExtensionW(path);
	return ext && lstrcmpiW(ext, L".png") == 0;
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

// Picks the bitmap/image file to try. Returns false when nothing is configured or
// configured files do not exist (caller then uses built-in image).
inline bool ResolveOrbPath(WCHAR* out /* >= MAX_PATH */, void (*getPresetFileName)(LPWSTR))
{
	WCHAR exeDir[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return false;
	if (!PathRemoveFileSpecW(exeDir)) return false;

	// 1) Custom file (BMP or PNG)
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
		dbgprintf(L"[Win7ExplorerRestorer] OrbFile '%s' not usable, trying preset", custom);
	}

	// 2) Preset directory
	WCHAR preset[MAX_PATH];
	if (getPresetFileName && ReadOrbRegString(L"OrbDirectory", preset, MAX_PATH))
	{
		if (!OrbPresetNameSafe(preset))
		{
			dbgprintf(L"[Win7ExplorerRestorer] OrbDirectory '%s' rejected", preset);
			return false;
		}
		WCHAR file[MAX_PATH];
		file[0] = L'\0';
		getPresetFileName(file);
		file[MAX_PATH - 1] = L'\0';
		if (!file[0]) return false;

		// Check .bmp first, then .png for presets
		WCHAR full[MAX_PATH * 3];
		if ((wnsprintfW(full, ARRAYSIZE(full), L"%s\\orbs\\%s\\%s.bmp", exeDir, preset, file) > 0)&&
			lstrlenW(full) < MAX_PATH && OrbIsRegularFile(full))
		{
			StringCchCopyW(out, MAX_PATH, full);
			return true;
		}

		if ((wnsprintfW(full, ARRAYSIZE(full), L"%s\\orbs\\%s\\%s.png", exeDir, preset, file) > 0)&&
			lstrlenW(full) < MAX_PATH && OrbIsRegularFile(full))
		{
			StringCchCopyW(out, MAX_PATH, full);
			return true;
		}
	}
	return false;
}

// Loads and validates a standard BMP file using Win32 GDI.
inline HBITMAP LoadOrbBitmapFromFile(const WCHAR* path, UINT fuLoad)
{
	ScopedGdiObject bmp(LoadImageW(nullptr, path, IMAGE_BITMAP, 0, 0, fuLoad | LR_LOADFROMFILE));
	if (!bmp.Get()) return nullptr;

	BITMAP bm;
	ZeroMemory(&bm, sizeof(bm));
	if (GetObjectW(bmp.Get(), sizeof(bm), &bm) != (int)sizeof(bm)) return nullptr;
	if (bm.bmWidth < 1 || bm.bmHeight < 1 || bm.bmWidth > kMaxOrbDim || bm.bmHeight > kMaxOrbDim)
	{
		dbgprintf(L"[Win7ExplorerRestorer] orb bitmap %ldx%ld rejected", bm.bmWidth, bm.bmHeight);
		return nullptr;
	}
	return (HBITMAP)bmp.Release();
}

// Decodes a PNG (or image file) with WIC, converting to 32bpp premultiplied BGRA (PBGRA)
// and returning a 32-bit top-down DIB section matching Windows shell requirements.
inline HBITMAP LoadOrbPngWithWic(const WCHAR* path)
{
	ScopedCoInit coInit(COINIT_APARTMENTTHREADED);
	if (!coInit.Succeeded())
	{
		dbgprintf(L"[Win7ExplorerRestorer] CoInitializeEx failed: 0x%08X", coInit.Result());
		return nullptr;
	}

	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.Put()));
	if (FAILED(hr) || !factory.Get())
	{
		dbgprintf(L"[Win7ExplorerRestorer] WIC factory creation failed: 0x%08X", hr);
		return nullptr;
	}

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(
		path,
		nullptr,
		GENERIC_READ,
		WICDecodeMetadataCacheOnDemand,
		decoder.Put());
	if (FAILED(hr) || !decoder.Get())
	{
		dbgprintf(L"[Win7ExplorerRestorer] CreateDecoderFromFilename failed: 0x%08X", hr);
		return nullptr;
	}

	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, frame.Put());
	if (FAILED(hr) || !frame.Get())
		return nullptr;

	UINT width = 0, height = 0;
	hr = frame->GetSize(&width, &height);
	if (FAILED(hr) || width < 1 || height < 1 || width > (UINT)kMaxOrbDim || height > (UINT)kMaxOrbDim)
	{
		dbgprintf(L"[Win7ExplorerRestorer] invalid image size %ux%u", width, height);
		return nullptr;
	}

	// Overflow guard for width * 4 * height
	if (width > (MAXDWORD / 4) || (width * 4) > (MAXDWORD / height))
		return nullptr;

	ComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(converter.Put());
	if (FAILED(hr) || !converter.Get())
		return nullptr;

	hr = converter->Initialize(
		frame.Get(),
		GUID_WICPixelFormat32bppPBGRA,
		WICBitmapDitherTypeNone,
		nullptr,
		0.0,
		WICBitmapPaletteTypeCustom);
	if (FAILED(hr))
	{
		dbgprintf(L"[Win7ExplorerRestorer] WIC format conversion to 32bppPBGRA failed: 0x%08X", hr);
		return nullptr;
	}

	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = (LONG)width;
	bi.bmiHeader.biHeight = -((LONG)height); // negative for top-down DIB
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void* pBits = nullptr;
	HDC hdc = GetDC(nullptr);
	HBITMAP hBitmap = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
	ReleaseDC(nullptr, hdc);

	if (!hBitmap || !pBits)
	{
		if (hBitmap) DeleteObject(hBitmap);
		return nullptr;
	}

	const UINT stride = width * 4;
	const UINT bufferSize = stride * height;
	hr = converter->CopyPixels(nullptr, stride, bufferSize, static_cast<BYTE*>(pBits));
	if (FAILED(hr))
	{
		dbgprintf(L"[Win7ExplorerRestorer] WIC CopyPixels failed: 0x%08X", hr);
		DeleteObject(hBitmap);
		return nullptr;
	}

	return hBitmap;
}

// Ensures that the loaded bitmap conforms to the Open-Shell 3-state vertical strip
// (ORB_STATE_NORMAL, ORB_STATE_HOT, ORB_STATE_PRESSED).
// If the image is a single frame or horizontal strip, it normalizes it into a vertical 3-state bitmap.
inline HBITMAP EnsureThreeStateOrb(HBITMAP hSrcBmp)
{
	if (!hSrcBmp) return nullptr;

	BITMAP bm;
	ZeroMemory(&bm, sizeof(bm));
	if (GetObjectW(hSrcBmp, sizeof(bm), &bm) != (int)sizeof(bm))
		return hSrcBmp;

	StartOrbMetrics metrics;
	if (!CalculateOrbMetrics(bm.bmWidth, bm.bmHeight, &metrics))
		return hSrcBmp;

	// If it is already a 3-state vertical strip, return as-is
	if (metrics.isThreeState && bm.bmHeight >= bm.bmWidth * 3)
		return hSrcBmp;

	// Normalize into 3-state vertical strip using RAII ScopedDC and ScopedGdiObject:
	// Frame 0: Idle/Normal (ORB_STATE_NORMAL)
	// Frame 1: Hover/Hot   (ORB_STATE_HOT)
	// Frame 2: Pressed     (ORB_STATE_PRESSED)
	ScopedDC hdcScreen(GetDC(nullptr), nullptr, false);
	if (!hdcScreen.Get()) return hSrcBmp;

	ScopedDC hdcSrc(CreateCompatibleDC(hdcScreen.Get()), nullptr, true);
	ScopedDC hdcDst(CreateCompatibleDC(hdcScreen.Get()), nullptr, true);
	if (!hdcSrc.Get() || !hdcDst.Get())
		return hSrcBmp;

	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = metrics.frameWidth;
	bi.bmiHeader.biHeight = -metrics.totalHeight; // Top-down DIB
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void* pDstBits = nullptr;
	HBITMAP hDstBmp = CreateDIBSection(hdcScreen.Get(), &bi, DIB_RGB_COLORS, &pDstBits, nullptr, 0);
	if (!hDstBmp)
		return hSrcBmp;

	ScopedGdiObject autoDst(hDstBmp);

	HGDIOBJ oldSrc = SelectObject(hdcSrc.Get(), hSrcBmp);
	HGDIOBJ oldDst = SelectObject(hdcDst.Get(), hDstBmp);

	if (!metrics.isThreeState)
	{
		// Single frame -> replicate to Normal (frame 0), Hot (frame 1), Pressed (frame 2)
		for (int state = 0; state < ORB_STATE_COUNT; ++state)
		{
			BitBlt(hdcDst.Get(), 0, state * metrics.frameHeight, metrics.frameWidth, metrics.frameHeight,
			       hdcSrc.Get(), 0, 0, SRCCOPY);
		}
	}
	else
	{
		// Horizontal 3-frame strip -> copy each horizontal frame to vertical slot
		for (int state = 0; state < ORB_STATE_COUNT; ++state)
		{
			BitBlt(hdcDst.Get(), 0, state * metrics.frameHeight, metrics.frameWidth, metrics.frameHeight,
			       hdcSrc.Get(), state * metrics.frameWidth, 0, SRCCOPY);
		}
	}

	SelectObject(hdcSrc.Get(), oldSrc);
	SelectObject(hdcDst.Get(), oldDst);

	DeleteObject(hSrcBmp);
	return (HBITMAP)autoDst.Release();
}

inline HBITMAP LoadOrbImageFromFile(const WCHAR* path, UINT fuLoad)
{
	HBITMAP hBmp = nullptr;
	if (OrbIsPng(path))
		hBmp = LoadOrbPngWithWic(path);
	else
		hBmp = LoadOrbBitmapFromFile(path, fuLoad);

	if (hBmp)
		hBmp = EnsureThreeStateOrb(hBmp);

	return hBmp;
}

// SEH-guarded unit of work. Plain struct + plain function: the __try lives in
// SafeInvokeCtx, which owns no C++ objects with destructors (rule C2712).
struct OrbRequest {
	UINT fuLoad;
	void (*getPresetFileName)(LPWSTR);
	HBITMAP result;
};

inline void OrbWork(OrbRequest* r)
{
	WCHAR path[MAX_PATH];
	if (ResolveOrbPath(path, r->getPresetFileName))
		r->result = LoadOrbImageFromFile(path, r->fuLoad);
}

} // namespace Win7ExplorerRestorer
