"""CI probe: which IID does the system stobject ask for when it starts the
network SSO, and which methods does it call on it? Loads stobject + our
cached pnidui (argv[1]) in-process, repoints the Windows To Go SSO slot to
{C2796011-...} like ExplorerPatcher, hooks stobject's CoCreateInstance IAT."""
import sys, ctypes, os, struct, uuid, time, threading
from ctypes import wintypes as W
import pefile
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.LoadLibraryExW.restype = W.HMODULE; k32.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
k32.GetProcAddress.restype = ctypes.c_void_p; k32.GetProcAddress.argtypes = [W.HMODULE, ctypes.c_char_p]
k32.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, W.DWORD, ctypes.POINTER(W.DWORD)]
ole = ctypes.OleDLL('ole32'); ole.CoInitializeEx(None, 2)
class GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]
    def __str__(s): return str(uuid.UUID(bytes_le=bytes(s)))
def g(s):
    x = GUID(); ole.CLSIDFromString(ctypes.c_wchar_p(s), ctypes.byref(x)); return x
NET = g("{C2796011-81BA-4148-8FCA-C6643245113F}")
pn = k32.LoadLibraryExW(os.path.abspath(sys.argv[1]), None, 8); print("pnidui", hex(pn or 0))
sp = r"C:\Windows\System32\stobject.dll"
st = k32.LoadLibraryExW(sp, None, 0); print("stobject", hex(st))
pe = pefile.PE(sp, fast_load=True)
base = st
wtg = uuid.UUID("4DC9C264-730E-4CF6-8374-70F079E4F82B").bytes_le
for sec in pe.sections:
    if sec.Name.startswith(b".rdata"):
        a, n = sec.VirtualAddress, sec.Misc_VirtualSize
        blob = ctypes.string_at(base + a, n)
        gi = blob.find(wtg); target = base + a + gi
        for i in range(0, n - 24, 8):
            p, sh, fl, fn_ = struct.unpack_from("<QiIQ", blob, i)
            if p == target:
                e = base + a + i; old = W.DWORD()
                k32.VirtualProtect(target, 16, 4, ctypes.byref(old)); ctypes.memmove(target, bytes(NET), 16)
                k32.VirtualProtect(e, 24, 4, ctypes.byref(old)); ctypes.memmove(e + 8, struct.pack("<iIQ", 1, 0, 0), 16)
                print("patched SSO entry", hex(a + i))
# hook IAT CoCreateInstance
orig_cci = k32.GetProcAddress(k32.LoadLibraryExW("combase.dll", None, 0x800), b"CoCreateInstance")
CCI = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(GUID), ctypes.c_void_p, W.DWORD, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))
real_cci = CCI(orig_cci)
DGCO = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(GUID), ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))
calls = []
keep = []
def make_proxy(obj):
    rvt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    F = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    thunks = []
    for i in range(24):
        def mk(i):
            def t(this, a, b, c):
                print("  proxy method", i, flush=True)
                return F(rvt[i])(obj, a, b, c)
            return F(t)
        thunks.append(mk(i))
    vt = (ctypes.c_void_p * 24)(*[ctypes.cast(t, ctypes.c_void_p) for t in thunks])
    pobj = ctypes.pointer(ctypes.c_void_p(ctypes.addressof(vt)))
    keep.extend([thunks, vt, pobj])
    return ctypes.cast(pobj, ctypes.c_void_p).value
def hook(clsid, outer, ctx, riid, ppv):
    s = str(clsid.contents)
    print("CoCreateInstance", s, "riid", str(riid.contents), flush=True)
    if s.lower() == "c2796011-81ba-4148-8fca-c6643245113f":
        fn = DGCO(k32.GetProcAddress(pn, b"DllGetClassObject"))
        cf = ctypes.c_void_p(); iid_cf = g("{00000001-0000-0000-C000-000000000046}")
        hr = fn(ctypes.byref(NET), ctypes.byref(iid_cf), ctypes.byref(cf))
        vt = ctypes.cast(ctypes.cast(cf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ci = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(vt[3])
        o = ctypes.c_void_p()
        hr = ci(cf, None, riid, ctypes.byref(o))
        print("  pnidui CreateInstance hr", hex(hr & 0xffffffff), flush=True)
        if hr >= 0:
            ppv[0] = make_proxy(o)
        return hr
    return real_cci(clsid, outer, ctx, riid, ppv)
cb = CCI(hook); keep.append(cb)
pe.parse_data_directories([pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in e.imports:
        if imp.name == b"CoCreateInstance":
            addr = base + imp.address - pe.OPTIONAL_HEADER.ImageBase
            old = W.DWORD(); k32.VirtualProtect(addr, 8, 4, ctypes.byref(old))
            ctypes.c_void_p.from_address(addr).value = ctypes.cast(cb, ctypes.c_void_p).value
            print("hooked IAT", e.dll.decode())
# create SysTray and start it
CLSID_SysTray = g("{35CEC8A3-2BE6-11D2-8773-92E220524153}")
IID_CT = g("{B722BCCB-4E68-101B-A2BC-00AA00404770}")
CGID = g("{000214D2-0000-0000-C000-000000000046}")
cf = ctypes.c_void_p()
fn = DGCO(k32.GetProcAddress(st, b"DllGetClassObject"))
print("st DGCO", hex(fn(ctypes.byref(CLSID_SysTray), ctypes.byref(g("{00000001-0000-0000-C000-000000000046}")), ctypes.byref(cf)) & 0xffffffff))
vt = ctypes.cast(ctypes.cast(cf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
ci = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(vt[3])
ct = ctypes.c_void_p()
print("SysTray CreateInstance", hex(ci(cf, None, ctypes.byref(IID_CT), ctypes.byref(ct)) & 0xffffffff), flush=True)
cvt = ctypes.cast(ctypes.cast(ct, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
Exec = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(GUID), W.DWORD, W.DWORD, ctypes.c_void_p, ctypes.c_void_p)(cvt[4])
u32 = ctypes.WinDLL('user32')
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
u32.DefWindowProcW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]; u32.DefWindowProcW.restype = ctypes.c_ssize_t
def wp(h, m, w, l): return u32.DefWindowProcW(h, m, w, l)
wpc = WNDPROC(wp); keep.append(wpc)
class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", W.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", W.HINSTANCE), ("hIcon", W.HICON), ("hCursor", ctypes.c_void_p), ("hbrBackground", W.HBRUSH), ("lpszMenuName", W.LPCWSTR), ("lpszClassName", W.LPCWSTR)]
u32.CreateWindowExW.restype = W.HWND
u32.CreateWindowExW.argtypes = [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.HWND, W.HMENU, W.HINSTANCE, ctypes.c_void_p]
for c in ("Shell_TrayWnd", "TrayNotifyWnd"):
    wc = WNDCLASSW(); wc.lpfnWndProc = wpc; wc.lpszClassName = c; u32.RegisterClassW(ctypes.byref(wc))
tw = u32.CreateWindowExW(0, "Shell_TrayWnd", "", 0x80000000, 0, 0, 100, 30, None, None, None, None)
tn = u32.CreateWindowExW(0, "TrayNotifyWnd", "", 0x40000000, 0, 0, 50, 30, tw, None, None, None)
print("fake tray", tw, tn, flush=True)
for cmd in (2, 4):
    print("Exec", cmd, hex(Exec(ct, ctypes.byref(CGID), cmd, 0, None, None) & 0xffffffff), flush=True)
    t0 = time.time(); msg = W.MSG()
    while time.time() - t0 < 6:
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(ctypes.byref(msg)); u32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.05)
print("done", flush=True)
os._exit(0)
