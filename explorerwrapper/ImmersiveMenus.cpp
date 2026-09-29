#include "ImmersiveMenus.h"
#include "SafeGuards.h"
#include "MinHook.h"

namespace ex7 {
void LogText(const wchar_t* text);
DWORD ReadAdvancedDwordPublic(const wchar_t* name, DWORD def);
namespace {

typedef HRESULT(*ApplyOwnerDraw_t)(HMENU, HWND, POINT*, unsigned int, void*);
HRESULT ApplyOwnerDraw_NoOp(HMENU, HWND, POINT*, unsigned int, void*) { return S_OK; }

HMODULE g_done[8];
void* g_tramp[8];

// 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??
// 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 4C 8B ?? ?? ?? ?? ?? 41 8B C1
const BYTE kPat[] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24,0,0,0,0,
	0x48,0x81,0xEC,0,0,0,0,0x48,0x8B,0x05,0,0,0,0,0x48,0x33,0xC4,0x48,0x89,0x85,0,0,0,0,0x4C,0x8B,0,0,0,0,0,0x41,0x8B,0xC1 };
const char kMask[] = "xxxxxxxxxxxxxxxxx????xxx????xxx????xxxxxx????xx?????xxx";

BYTE* Find(BYTE* p, DWORD n)
{
	const DWORD m = sizeof(kPat);
	for (DWORD i = 0; i + m <= n; ++i) {
		DWORD k = 0;
		for (; k < m; ++k) if (kMask[k] == 'x' && p[i + k] != kPat[k]) break;
		if (k == m) return p + i;
	}
	return nullptr;
}

struct Ctx { HMODULE m; const wchar_t* name; };

void Unsafe(Ctx* c)
{
	BYTE* base = (BYTE*)c->m;
	IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	BYTE* hit = nullptr;
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections && !hit; ++i)
		if (!memcmp(sec[i].Name, ".text", 5)) hit = Find(base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize);
	wchar_t l[200];
	if (!hit) { wnsprintfW(l, 200, L"[ex7][menus] %s: ApplyOwnerDrawToMenu not found (menu stays as is)", c->name); LogText(l); return; }
	MH_Initialize();
	int slot = -1;
	for (int i = 0; i < 8; ++i) if (!g_done[i]) { slot = i; break; }
	if (slot < 0) return;
	MH_STATUS a = MH_CreateHook(hit, (void*)ApplyOwnerDraw_NoOp, &g_tramp[slot]);
	MH_STATUS b = a == MH_OK ? MH_EnableHook(hit) : a;
	g_done[slot] = c->m;
	wnsprintfW(l, 200, L"[ex7][menus] %s: immersive menus disabled at +0x%X (%d/%d)", c->name, (DWORD)(hit - base), a, b);
	LogText(l);
}

} // namespace

void ClassicMenusFor(HMODULE m, const wchar_t* name)
{
	if (!m) return;
	for (HMODULE d : g_done) if (d == m) return;
	if (ReadAdvancedDwordPublic(L"ClassicTrayMenus", 1) == 0) return;
	Ctx c = { m, name };
	SafeInvokeCtx<Ctx>(L"ClassicMenusFor", Unsafe, &c);
}

} // namespace ex7
