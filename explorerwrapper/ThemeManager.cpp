#include "ThemeManager.h"
#include "dbgprint.h"
#include "pathcch.h"
#include "OSVersion.h"
#include "RegistryManager.h"
#include "resource.h"
#include "shlobj.h"
#include <stdarg.h>

// Extracts the embedded, self-contained 7explorer theme (generated at
// build time from our own assets) to %LocalAppData%\7explorer\theme
// if the file is not present yet. Returns true when the theme file
// exists afterwards. No user interaction, no downloads, silent.
// Theme diagnostics go to OutputDebugString AND %LocalAppData%\7explorer
// \theme.log so users can attach the log to bug reports without any
// extra steps. Append-only, tiny size guard.
static void ThemeLog(const wchar_t *fmt, ...)
{
	WCHAR msg[512];
	va_list argp;
	va_start(argp, fmt);
	int cnt = wvsprintfW(msg, fmt, argp);
	va_end(argp);
	if (cnt <= 0) return;
	OutputDebugStringW(msg);
	WCHAR szBase[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szBase)))
		return;
	WCHAR szDir[MAX_PATH];
	wsprintfW(szDir, L"%s\\7explorer", szBase);
	CreateDirectoryW(szDir, NULL);
	WCHAR szLog[MAX_PATH];
	wsprintfW(szLog, L"%s\\theme.log", szDir);
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (GetFileAttributesExW(szLog, GetFileExInfoStandard, &info))
	{
		// rotate at 256 KB
		if (info.nFileSizeLow > (256 << 10))
			DeleteFileW(szLog);
	}
	HANDLE h = CreateFileW(szLog, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	SYSTEMTIME st; GetLocalTime(&st);
	WCHAR line[560];
	int len = wsprintfW(line, L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
	DWORD written;
	// convert to UTF-8 for portability
	int n = WideCharToMultiByte(CP_UTF8, 0, line, len, NULL, 0, NULL, NULL);
	CHAR buf8[1200];
	if (n > 0 && n < (int)sizeof(buf8))
	{
		WideCharToMultiByte(CP_UTF8, 0, line, len, buf8, n, NULL, NULL);
		WriteFile(h, buf8, n, &written, NULL);
	}
	CloseHandle(h);
}

static BOOL EnsureEmbeddedThemeFile(wchar_t *szOut, DWORD cchOut)
{
	WCHAR szBase[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, szBase)))
		return FALSE;
	WCHAR szDir[MAX_PATH];
	wsprintfW(szDir, L"%s\\7explorer", szBase);
	CreateDirectoryW(szDir, NULL);
	wsprintfW(szDir, L"%s\\7explorer\\theme", szBase);
	CreateDirectoryW(szDir, NULL);
	wsprintfW(szOut, L"%s\\aero.msstyles", szDir);

	// already extracted? keep user modifications if any
	DWORD attr = GetFileAttributesW(szOut);
	if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
		return TRUE;

	HMODULE hSelf = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&EnsureEmbeddedThemeFile, &hSelf);
	if (!hSelf)
		hSelf = GetModuleHandleW(L"explorerwrapper.dll");
	if (!hSelf)
		return FALSE;
	HRSRC hRes = FindResourceW(hSelf, MAKEINTRESOURCEW(IDR_BUILTIN_THEME),
		RT_RCDATA);
	if (!hRes) return FALSE;
	HGLOBAL hData = LoadResource(hSelf, hRes);
	if (!hData) return FALSE;
	DWORD size = SizeofResource(hSelf, hRes);
	void *blob = LockResource(hData);
	if (!blob || !size) return FALSE;
	HANDLE hFile = CreateFileW(szOut, GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return FALSE;
	DWORD written = 0;
	WriteFile(hFile, blob, size, &written, NULL);
	CloseHandle(hFile);
	ThemeLog(L"7explorer: extracted embedded theme to %s (%u bytes)\n",
		szOut, written);
	return written == size;
}

decltype(GetThemeDefaults) GetThemeDefaults = 0;
decltype(LoaderLoadTheme) LoaderLoadTheme = 0;
decltype(OpenThemeDataFromFile) OpenThemeDataFromFile = 0;

UXTHEMEFILE *g_loadedTheme = 0;

void FreeTheme(UXTHEMEFILE* file)
{
	if (file)
	{
		if (file->sharableSectionView)
		{
			UnmapViewOfFile(file->sharableSectionView);
		}
		if (file->nsSectionView)
		{
			UnmapViewOfFile(file->nsSectionView);
		}

		CloseHandle(file->hNsSection);
		CloseHandle(file->hSharableSection);

		free(file);
	}
}

DWORD WINAPI DelayFreeThread(LPVOID lParam)
{
	//wait 1 sec
	Sleep(1000);

	FreeTheme((UXTHEMEFILE*)lParam);

	return 0;
}

void ThemeManagerInitialize()
{
	//dont bother error checking, if u dont got uxtheme, ur system is prob already messed up and theres no saving u
	HMODULE hUxTheme = GetModuleHandleW(L"uxtheme.dll");
	GetThemeDefaults = (decltype(GetThemeDefaults))GetProcAddress(hUxTheme, (LPCSTR)7);
	LoaderLoadTheme = (decltype(LoaderLoadTheme))GetProcAddress(hUxTheme, (LPCSTR)92);
	OpenThemeDataFromFile = (decltype(OpenThemeDataFromFile))GetProcAddress(hUxTheme, (LPCSTR)16);

	dbgprintf(L"GetThemeDefaults %x LoaderLoadTheme %x OpenThemeDataFromFile %x\n", GetThemeDefaults, LoaderLoadTheme, OpenThemeDataFromFile);

	// get directory of explorer.exe (NOT the working directory)
	WCHAR szExeDir[MAX_PATH];
	GetModuleFileNameW(NULL, szExeDir, MAX_PATH);
	WCHAR *backslash = StrRChrW(szExeDir, NULL, L'\\');
	if (*backslash == L'\\')
		*backslash = L'\0';

	WCHAR szThemeName[MAX_PATH];
	LSTATUS res = g_registry.QueryValue(L"Theme", (LPBYTE)szThemeName, sizeof(szThemeName));
	if (!*szThemeName || ERROR_SUCCESS != res)
		StringCchCopyW(szThemeName, MAX_PATH, L"aero");

	ThemeLog(L"theme name: %s", szThemeName);

	WCHAR szThemePath[MAX_PATH * 2];
	wsprintfW(
		szThemePath,
		L"%s\\theme\\%s.msstyles",
		szExeDir,
		szThemeName
	);

	ThemeLog(L"theme path: %s", szThemePath);

	auto hr = S_OK;
	DWORD attr = GetFileAttributesW(szThemePath);
	if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		// The theme file is not installed next to explorer.exe. Use the
		// embedded, self-contained theme instead: it is extracted to
		// %LocalAppData%\7explorer\theme and loaded from there.
		WCHAR szEmbedded[MAX_PATH];
		if (EnsureEmbeddedThemeFile(szEmbedded, ARRAYSIZE(szEmbedded)))
		hr = LoadThemeFile(szEmbedded);
		else
		ThemeLog(L"embedded theme extraction FAILED\n");
	}
	else
	{
		hr = LoadThemeFile(szThemePath);
		if (hr != S_OK)
		{
			ThemeLog(L"LOADTHEMEFILE %x for %s, trying embedded\n",
				hr, szThemePath);
			WCHAR szEmbedded[MAX_PATH];
			if (EnsureEmbeddedThemeFile(szEmbedded, ARRAYSIZE(szEmbedded)))
				hr = LoadThemeFile(szEmbedded);
		}
	}

	if (hr != S_OK)
		ThemeLog(L"LOADTHEMEFILE FAILED %x (keeping classic theme)\n", hr);
}

HRESULT LoadThemeFile(wchar_t *Path)
{
	HRESULT hr = S_OK;

	if (g_loadedTheme)
	{
		//create delay free thread
		//CreateThread(0,0, DelayFreeThread,g_loadedTheme,0,0);
		FreeTheme(g_loadedTheme);
		g_loadedTheme = 0;
	}

	g_loadedTheme = (UXTHEMEFILE *)malloc(sizeof(UXTHEMEFILE));
	ZeroMemory(g_loadedTheme, sizeof(UXTHEMEFILE));

	WCHAR szColor[MAX_PATH];
	WCHAR szSize[MAX_PATH];

	ThemeLog(L"LoadThemeFile: %s\n", Path);
	hr = GetThemeDefaults(
		Path,
		szColor,
		ARRAYSIZE(szColor),
		szSize,
		ARRAYSIZE(szSize)
	);
	if (hr != S_OK)
	{
		ThemeLog(L"GetThemeDefaults failed %x\n", hr);
		if (g_loadedTheme)
		{
			if (g_loadedTheme->sharableSectionView)
			{
				UnmapViewOfFile(g_loadedTheme->sharableSectionView);
			}
			if (g_loadedTheme->nsSectionView)
			{
				UnmapViewOfFile(g_loadedTheme->nsSectionView);
			}
			CloseHandle(g_loadedTheme->hNsSection);
			CloseHandle(g_loadedTheme->hSharableSection);
			//free(g_loadedTheme);
			g_loadedTheme = 0;
			dbgprintf(L"LoadTHemeFile failed 1");
		}
		return hr;
	}

	HANDLE hSharable, hNonSharable;
	if (g_osVersion.BuildNumber() < 20000
		? LoaderLoadTheme(0LL, 0LL, Path, szColor, szSize, &hSharable, 0LL, 0, &hNonSharable, 0LL, 0, 0LL, 0LL, 0, 0, 0)
		: ((LoaderLoadTheme_t_win11)LoaderLoadTheme)(
			0LL,
			0LL,
			Path,
			szColor,
			szSize,
			&hSharable,
			0LL,
			0,
			&hNonSharable,
			0LL,
			0,
			0LL,
			0LL,
			0,
			0))
	{
		if (g_loadedTheme)
		{
			if (g_loadedTheme->sharableSectionView)
			{
				UnmapViewOfFile(g_loadedTheme->sharableSectionView);
			}
			if (g_loadedTheme->nsSectionView)
			{
				UnmapViewOfFile(g_loadedTheme->nsSectionView);
			}
			CloseHandle(g_loadedTheme->hNsSection);
			CloseHandle(g_loadedTheme->hSharableSection);
			//free(g_loadedTheme);
			g_loadedTheme = 0;
			dbgprintf(L"LoadTHemeFile failed 2");
		}
		return hr;
	}

	ThemeLog(L"theme sections loaded OK\n");
	memcpy(g_loadedTheme->header, "thmfile", 7);
	memcpy(g_loadedTheme->end, "end", 3);
	g_loadedTheme->sharableSectionView = MapViewOfFile(hSharable, 4, 0, 0, 0);
	g_loadedTheme->hSharableSection = hSharable;
	g_loadedTheme->nsSectionView = MapViewOfFile(hNonSharable, 4, 0, 0, 0);
	g_loadedTheme->hNsSection = hNonSharable;

	return S_OK;
}
