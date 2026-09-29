#include "NotifyIconsDialog.h"
#include "SafeGuards.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

namespace ex7 {
void LogText(const wchar_t* text);
namespace {

// Win7 private interfaces (explorer 6.1), as documented by the community
// (e.g. geoffchappell / "TrayNotify" tools).
struct NOTIFYITEM {
	PWSTR pszExeName; PWSTR pszTip; HICON hIcon; HWND hWnd; DWORD dwPreference; UINT uID; GUID guidItem;
};
struct INotificationCB : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, NOTIFYITEM* item) = 0;
};
struct ITrayNotify7 : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* cb) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPreference(const NOTIFYITEM* item) = 0;
	virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enable) = 0;
};
const CLSID kClsidTrayNotify = { 0x25DEAD04, 0x1EAC, 0x4911, { 0x9E, 0x3A, 0xAD, 0x0A, 0x4A, 0xB5, 0x60, 0xFD } };
const IID kIidTrayNotify7 = { 0xFB852B2C, 0x6BAD, 0x4605, { 0x95, 0x51, 0xF1, 0x5F, 0x87, 0x83, 0x09, 0x35 } };
const IID kIidNotificationCB = { 0xD782CCBA, 0xAFB0, 0x43F1, { 0x94, 0xDB, 0xFD, 0xA3, 0x77, 0x9E, 0xAC, 0xCB } };

struct Item { NOTIFYITEM ni; wchar_t exe[MAX_PATH]; wchar_t tip[128]; };
const int kMax = 128;
Item g_items[kMax];
int g_count = 0;
HWND g_dlg = nullptr, g_list = nullptr, g_combo = nullptr, g_check = nullptr;
ITrayNotify7* g_tn = nullptr;
volatile LONG g_open = 0;

bool Italian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN; }
const wchar_t* T(const wchar_t* it, const wchar_t* en) { return Italian() ? it : en; }

class CB : public INotificationCB {
public:
	STDMETHODIMP QueryInterface(REFIID r, void** p) override {
		if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, kIidNotificationCB)) { *p = this; return S_OK; }
		*p = nullptr; return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP Notify(ULONG, NOTIFYITEM* ni) override {
		__try {
			if (!ni) return S_OK;
			for (int i = 0; i < g_count; ++i) // update existing
				if (g_items[i].ni.hWnd == ni->hWnd && g_items[i].ni.uID == ni->uID &&
					IsEqualGUID(g_items[i].ni.guidItem, ni->guidItem)) { Fill(g_items[i], ni); return S_OK; }
			if (g_count < kMax) Fill(g_items[g_count++], ni);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return S_OK;
	}
	static void Fill(Item& it, const NOTIFYITEM* ni) {
		it.ni = *ni;
		lstrcpynW(it.exe, ni->pszExeName ? ni->pszExeName : L"", MAX_PATH);
		lstrcpynW(it.tip, ni->pszTip ? ni->pszTip : L"", 128);
		it.ni.pszExeName = it.exe; it.ni.pszTip = it.tip;
		it.ni.hIcon = ni->hIcon ? CopyIcon(ni->hIcon) : nullptr;
	}
};
CB g_cb;

enum { IDC_LIST = 100, IDC_COMBO, IDC_CHECK, IDC_OK, IDC_CANCEL };

const wchar_t* PrefText(DWORD p)
{
	switch (p) {
	case 2: return T(L"Mostra icona e notifiche", L"Show icon and notifications");
	case 1: return T(L"Nascondi icona e notifiche", L"Hide icon and notifications");
	default: return T(L"Mostra solo notifiche", L"Only show notifications");
	}
}

void Fill()
{
	ListView_DeleteAllItems(g_list);
	HIMAGELIST il = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, g_count, 4);
	HIMAGELIST old = ListView_SetImageList(g_list, il, LVSIL_SMALL);
	if (old) ImageList_Destroy(old);
	for (int i = 0; i < g_count; ++i) {
		int img = g_items[i].ni.hIcon ? ImageList_AddIcon(il, g_items[i].ni.hIcon) : -1;
		const wchar_t* name = g_items[i].tip[0] ? g_items[i].tip : PathFindFileNameW(g_items[i].exe);
		wchar_t first[128]; lstrcpynW(first, name, 128);
		for (wchar_t* q = first; *q; ++q) if (*q == L'\r' || *q == L'\n') { *q = 0; break; }
		LVITEMW lv = {}; lv.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM; lv.iItem = i;
		lv.pszText = first; lv.iImage = img; lv.lParam = i;
		int row = ListView_InsertItem(g_list, &lv);
		ListView_SetItemText(g_list, row, 1, (LPWSTR)PrefText(g_items[i].ni.dwPreference));
	}
}

int Selected() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }

void Layout(HWND h)
{
	RECT rc; GetClientRect(h, &rc);
	int w = rc.right, hh = rc.bottom, m = 12;
	MoveWindow(g_list, m, m, w - 2 * m, hh - 120, TRUE);
	MoveWindow(g_combo, m, hh - 100, 260, 200, TRUE);
	MoveWindow(g_check, m, hh - 68, w - 2 * m, 22, TRUE);
	MoveWindow(GetDlgItem(h, IDC_OK), w - 2 * (90 + m), hh - 36, 90, 26, TRUE);
	MoveWindow(GetDlgItem(h, IDC_CANCEL), w - (90 + m), hh - 36, 90, 26, TRUE);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	__try {
		switch (msg) {
		case WM_SIZE: Layout(h); return 0;
		case WM_NOTIFY: {
			NMHDR* n = (NMHDR*)l;
			if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED) {
				int s = Selected();
				if (s >= 0) {
					DWORD p = g_items[s].ni.dwPreference;
					SendMessageW(g_combo, CB_SETCURSEL, p == 2 ? 0 : p == 1 ? 1 : 2, 0);
				}
			}
			return 0;
		}
		case WM_COMMAND:
			if (LOWORD(w) == IDC_COMBO && HIWORD(w) == CBN_SELCHANGE) {
				int s = Selected();
				if (s >= 0) {
					int c = (int)SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
					g_items[s].ni.dwPreference = c == 0 ? 2 : c == 1 ? 1 : 0;
					ListView_SetItemText(g_list, s, 1, (LPWSTR)PrefText(g_items[s].ni.dwPreference));
				}
			} else if (LOWORD(w) == IDC_CHECK) {
				BOOL all = SendMessageW(g_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
				EnableWindow(g_list, !all); EnableWindow(g_combo, !all);
			} else if (LOWORD(w) == IDC_OK) {
				if (g_tn) {
					for (int i = 0; i < g_count; ++i) g_tn->SetPreference(&g_items[i].ni);
					g_tn->EnableAutoTray(SendMessageW(g_check, BM_GETCHECK, 0, 0) != BST_CHECKED);
				}
				DestroyWindow(h);
			} else if (LOWORD(w) == IDC_CANCEL) DestroyWindow(h);
			return 0;
		case WM_CLOSE: DestroyWindow(h); return 0;
		case WM_DESTROY: PostQuitMessage(0); return 0;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return DefWindowProcW(h, msg, w, l);
}

HWND Ctl(HWND p, DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT f)
{
	HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, p, (HMENU)(INT_PTR)id, nullptr, nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)f, FALSE);
	return c;
}

void RunUnsafe()
{
	HRESULT hr = CoCreateInstance(kClsidTrayNotify, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER, kIidTrayNotify7, (void**)&g_tn);
	wchar_t l[160]; wnsprintfW(l, 160, L"[ex7][notifyicons] TrayNotify hr=0x%08X", (DWORD)hr); LogText(l);
	if (FAILED(hr) || !g_tn) {
		g_tn = nullptr;
		ShellExecuteW(nullptr, nullptr, L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	g_count = 0;
	g_tn->RegisterCallback(&g_cb);
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);
	WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = L"Ex7NotifyIconsWnd";
	RegisterClassW(&wc);
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
	g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, T(L"Icone dell'area di notifica", L"Notification Area Icons"),
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, 620, 480, nullptr, nullptr, wc.hInstance, nullptr);
	if (!g_dlg) { if (font) DeleteObject(font); return; }
	g_list = Ctl(g_dlg, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IDC_LIST, font);
	ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	LVCOLUMNW col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = (LPWSTR)T(L"Icone", L"Icons"); col.cx = 320; ListView_InsertColumn(g_list, 0, &col);
	col.pszText = (LPWSTR)T(L"Comportamento", L"Behaviors"); col.cx = 250; ListView_InsertColumn(g_list, 1, &col);
	g_combo = Ctl(g_dlg, 0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_COMBO, font);
	const DWORD prefs[3] = { 2, 1, 0 };
	for (DWORD p : prefs) SendMessageW(g_combo, CB_ADDSTRING, 0, (LPARAM)PrefText(p));
	g_check = Ctl(g_dlg, 0, WC_BUTTONW, T(L"Mostra sempre tutte le icone e le notifiche sulla barra delle applicazioni",
		L"Always show all icons and notifications on the taskbar"), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHECK, font);
	Ctl(g_dlg, 0, WC_BUTTONW, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_OK, font);
	Ctl(g_dlg, 0, WC_BUTTONW, T(L"Annulla", L"Cancel"), BS_PUSHBUTTON | WS_TABSTOP, IDC_CANCEL, font);
	// the callback enumerates the current icons; give it a moment
	DWORD t0 = GetTickCount(); MSG m;
	while (GetTickCount() - t0 < 400) {
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
		Sleep(20);
	}
	DWORD autoTray = 1, cb = sizeof(autoTray);
	RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"EnableAutoTray", RRF_RT_REG_DWORD, nullptr, &autoTray, &cb);
	SendMessageW(g_check, BM_SETCHECK, autoTray ? BST_UNCHECKED : BST_CHECKED, 0);
	EnableWindow(g_list, autoTray != 0); EnableWindow(g_combo, autoTray != 0);
	Fill();
	Layout(g_dlg);
	ShowWindow(g_dlg, SW_SHOWNORMAL);
	SetForegroundWindow(g_dlg);
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(g_dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
	}
	g_tn->RegisterCallback(nullptr);
	g_tn->Release(); g_tn = nullptr;
	for (int i = 0; i < g_count; ++i) if (g_items[i].ni.hIcon) DestroyIcon(g_items[i].ni.hIcon);
	g_count = 0; g_dlg = nullptr;
	if (font) DeleteObject(font);
}

DWORD WINAPI Thread(LPVOID)
{
	HRESULT hi = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	SafeInvoke(L"NotifyIconsDialog", RunUnsafe);
	if (SUCCEEDED(hi)) CoUninitialize();
	InterlockedExchange(&g_open, 0);
	return 0;
}

} // namespace

bool ShowNotifyIconsDialog()
{
	if (InterlockedCompareExchange(&g_open, 1, 0) != 0) {
		HWND h = FindWindowW(L"Ex7NotifyIconsWnd", nullptr);
		if (h) SetForegroundWindow(h);
		return true;
	}
	HANDLE t = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
	if (!t) { InterlockedExchange(&g_open, 0); return false; }
	CloseHandle(t);
	return true;
}

} // namespace ex7
