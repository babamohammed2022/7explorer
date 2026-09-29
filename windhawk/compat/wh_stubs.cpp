// wh_stubs.cpp — CI COMPILE-CHECK STUB ONLY (see windhawk_api.h note).
// No-op implementations of the Windhawk API surface, good enough to
// compile (and syntax-link) the PoC mods on any Windows CI runner.
#include "windhawk_api.h"

#include <cstdarg>
#include <cstdio>

void Wh_Log(const wchar_t*, ...) {}
BOOL Wh_SetFunctionHook(void*, void*, void**) { return TRUE; }
PCWSTR Wh_GetStringSetting(LPCWSTR) { return nullptr; }
void Wh_FreeStringSetting(PCWSTR) {}
