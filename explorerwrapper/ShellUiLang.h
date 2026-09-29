#pragma once
#include "common.h"

// Effective UI language of the private shell (test40): the
// WIN7EXPLORERRESTORER_UI_LANG override set by the shell switcher at launch
// time wins, otherwise the primary system UI language. Process-local only -
// no Windows-wide setting is ever read for writing, let alone changed.
// Index order (must match TX argument order below):
//   0=en 1=it 2=de 3=es 4=fr 5=ja 6=pl 7=pt-BR 8=ru 9=zh-CN
// NOTE: no C++ try/catch here - the wrapper builds with C++ exceptions
// disabled; this is pure straight-line code that cannot throw.

struct ShellUiLangCode { const wchar_t* name; int idx; };
static const ShellUiLangCode kShellUiLangCodes[] = {
	{ L"en", 0 }, { L"en-US", 0 }, { L"eng", 0 },
	{ L"it", 1 }, { L"it-IT", 1 }, { L"ita", 1 },
	{ L"de", 2 }, { L"de-DE", 2 }, { L"deu", 2 },
	{ L"es", 3 }, { L"es-ES", 3 }, { L"esp", 3 },
	{ L"fr", 4 }, { L"fr-FR", 4 }, { L"fra", 4 }, { L"fre", 4 },
	{ L"ja", 5 }, { L"ja-JP", 5 }, { L"jpn", 5 },
	{ L"pl", 6 }, { L"pl-PL", 6 }, { L"pol", 6 },
	{ L"pt", 7 }, { L"pt-BR", 7 }, { L"ptbr", 7 }, { L"por", 7 }, { L"pt-PT", 7 },
	{ L"ru", 8 }, { L"ru-RU", 8 }, { L"rus", 8 },
	{ L"zh", 9 }, { L"zh-CN", 9 }, { L"zhcn", 9 }, { L"chs", 9 },
	{ L"cht", 9 }, { L"zh-TW", 9 }, { L"zh-HK", 9 },
};

// Recognized override codes (full BCP-47 plus the short/legacy aliases the
// headless CLI may inherit). Unknown text is NOT an override.
inline bool ShellUiLangParse(const wchar_t* l, int* idx)
{
	for (size_t i = 0; i < ARRAYSIZE(kShellUiLangCodes); ++i) {
		if (lstrcmpiW(l, kShellUiLangCodes[i].name) == 0) {
			if (idx) *idx = kShellUiLangCodes[i].idx;
			return true;
		}
	}
	return false;
}

inline int ShellUiLangIndex()
{
	// Cached: the child environment is fixed at CreateProcess time, so the
	// value cannot change under us (a benign recompute on thread races).
	static int cached = -1;
	if (cached >= 0) return cached;
	int idx = 0;
	WCHAR l[16];
	DWORD n = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", l, 15);
	bool fromEnv = false;
	if (n > 0 && n < 15) {
		l[n] = 0;
		fromEnv = ShellUiLangParse(l, &idx);
	}
	if (!fromEnv) {
		switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
		case LANG_ITALIAN:    idx = 1; break;
		case LANG_GERMAN:     idx = 2; break;
		case LANG_SPANISH:    idx = 3; break;
		case LANG_FRENCH:     idx = 4; break;
		case LANG_JAPANESE:   idx = 5; break;
		case LANG_POLISH:     idx = 6; break;
		case LANG_PORTUGUESE: idx = 7; break; // pt-PT shares pt-BR
		case LANG_RUSSIAN:    idx = 8; break;
		case LANG_CHINESE:    idx = 9; break; // Traditional shares Simplified
		default:              idx = 0; break;
		}
	}
	cached = idx;
	return idx;
}

// BCP-47 code of the effective language ("en-US" ... "zh-CN").
inline const wchar_t* ShellUiLangCode()
{
	static const wchar_t* codes[] = {
		L"en-US", L"it-IT", L"de-DE", L"es-ES", L"fr-FR",
		L"ja-JP", L"pl-PL", L"pt-BR", L"ru-RU", L"zh-CN"
	};
	int i = ShellUiLangIndex();
	return (i >= 0 && i < 10) ? codes[i] : codes[0];
}

// True when WIN7EXPLORERRESTORER_UI_LANG names a supported language.
inline bool ShellUiLangOverridden()
{
	WCHAR l[16];
	DWORD n = GetEnvironmentVariableW(L"WIN7EXPLORERRESTORER_UI_LANG", l, 15);
	if (n == 0 || n >= 15) return false;
	l[n] = 0;
	return ShellUiLangParse(l, nullptr);
}

// Ten-language string picker (replaces the old bilingual T(it,en)).
// Argument order MUST match the index order: en it de es fr ja pl pt-BR ru zh-CN.
inline const wchar_t* TX(const wchar_t* en, const wchar_t* it, const wchar_t* de,
	const wchar_t* es, const wchar_t* fr, const wchar_t* ja, const wchar_t* pl,
	const wchar_t* ptbr, const wchar_t* ru, const wchar_t* zhcn)
{
	const wchar_t* s[] = { en, it, de, es, fr, ja, pl, ptbr, ru, zhcn };
	int i = ShellUiLangIndex();
	if (i < 0 || i > 9 || !s[i]) return en;
	return s[i];
}
