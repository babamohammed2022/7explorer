// windhawk_api.h — CI COMPILE-CHECK STUB ONLY.
// The real header ships with Windhawk (ramensoftware.com) and is used when
// Windhawk compiles the mod on the target machine. This stub only exposes
// the tiny API surface our two PoC mods use, so the CI can verify the
// sources compile cleanly for x64 without Windhawk installed.
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void Wh_Log(const wchar_t* fmt, ...);
BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction,
                        void** originalFunction);
PCWSTR Wh_GetStringSetting(LPCWSTR valueName);
void Wh_FreeStringSetting(PCWSTR string);

#ifdef __cplusplus
}  // extern "C"
#endif
