"""CI probe: can pnidui.dll 10.0.22621.5415 (symbol server) run on this Windows?
Downloads nothing itself; argv[1] = path of the downloaded dll."""
import sys, ctypes, hashlib, os
from ctypes import wintypes as W
import pefile
p = sys.argv[1]
print("sha256", hashlib.sha256(open(p, 'rb').read()).hexdigest())
pe = pefile.PE(p)
print("imports:")
for e in pe.DIRECTORY_ENTRY_IMPORT:
    print("  ", e.dll.decode(), len(e.imports))
for e in getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
    print("  delay", e.dll.decode())
print("resources:")
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    n = t.name or t.id
    print("  type", n, len(t.directory.entries))
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.LoadLibraryExW.restype = W.HMODULE
k32.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
for d in [e.dll.decode() for e in pe.DIRECTORY_ENTRY_IMPORT]:
    h = k32.LoadLibraryExW(d, None, 0x800)
    if not h: print("  MISSING import dll", d, ctypes.get_last_error())
ole = ctypes.OleDLL('ole32')
ole.CoInitializeEx(None, 2)
if len(sys.argv) > 2:
    os.makedirs(os.path.join(os.path.dirname(os.path.abspath(p)), "en-US"), exist_ok=True)
    import shutil; shutil.copy(sys.argv[2], os.path.join(os.path.dirname(os.path.abspath(p)), "en-US", "pnidui.dll.mui"))
h = k32.LoadLibraryExW(os.path.abspath(p), None, 0x8)
print("LoadLibraryEx", hex(h or 0), ctypes.get_last_error())
if not h: sys.exit(0)
class GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]
def g(s):
    x = GUID(); ole.CLSIDFromString(ctypes.c_wchar_p(s), ctypes.byref(x)); return x
clsid = g("{C2796011-81BA-4148-8FCA-C6643245113F}")
iid_cf = g("{00000001-0000-0000-C000-000000000046}")
iid_ct = g("{B722BCCB-4E68-101B-A2BC-00AA00404770}")
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [W.HMODULE, ctypes.c_char_p]
fn = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.POINTER(GUID), ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(k32.GetProcAddress(h, b"DllGetClassObject"))
cf = ctypes.c_void_p()
hr = fn(ctypes.byref(clsid), ctypes.byref(iid_cf), ctypes.byref(cf))
print("DllGetClassObject hr", hex(hr & 0xffffffff))
if hr < 0: sys.exit(0)
vt = ctypes.cast(ctypes.cast(cf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
ci = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(vt[3])
iid_unk = g("{00000000-0000-0000-C000-000000000046}")
obj = ctypes.c_void_p()
hr = ci(cf, None, ctypes.byref(iid_unk), ctypes.byref(obj))
print("CreateInstance(IUnknown) hr", hex(hr & 0xffffffff))
if hr >= 0:
    ovt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    qi = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(ovt[0])
    for name, s_ in [("IOleCommandTarget","{B722BCCB-4E68-101B-A2BC-00AA00404770}"),("IObjectWithSite","{FC4801A3-2BA9-11CF-A229-00AA003D7352}"),
                     ("IOleWindow","{00000114-0000-0000-C000-000000000046}"),("IServiceProvider","{6D5140C1-7436-11CE-8034-00AA006009FA}"),
                     ("IPersist","{0000010C-0000-0000-C000-000000000046}"),("IMarshal","{00000003-0000-0000-C000-000000000046}"),
                     ("IAgileObject","{94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}"),("IInspectable","{AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90}"),
                     ("IShellServiceObject?","{D5E7D6E8-A7A1-4C75-A4C7-4C4B4E3C1E1A}"),("IDispatch","{00020400-0000-0000-C000-000000000046}")]:
        o = ctypes.c_void_p()
        r = qi(obj, ctypes.byref(g(s_)), ctypes.byref(o))
        print("  QI", name, hex(r & 0xffffffff))
u32 = ctypes.WinDLL('user32')
for rid in list(range(1, 20)) + [100, 200, 1000] + list(range(2000, 2010)) + list(range(3000, 3010)):
    buf = ctypes.create_unicode_buffer(256)
    n = u32.LoadStringW(W.HMODULE(h), rid, buf, 256)
    print("LoadString", rid, n)

# --- system stobject: EP-style SSO table lookup (WindowsToGo SSO entry)
import struct, uuid
sp = r"C:\Windows\System32\stobject.dll"
if os.path.exists(sp):
    st = pefile.PE(sp)
    data = st.get_memory_mapped_image()
    base = st.OPTIONAL_HEADER.ImageBase
    wtg = uuid.UUID("4DC9C264-730E-4CF6-8374-70F079E4F82B").bytes_le
    for sec in st.sections:
        if sec.Name.startswith(b".rdata"):
            a, n = sec.VirtualAddress, sec.Misc_VirtualSize
            blob = data[a:a+n]
            gi = blob.find(wtg)
            print("stobject WindowsToGo GUID rva", hex(a+gi) if gi >= 0 else None)
            if gi >= 0:
                target = base + a + gi
                for i in range(0, n - 24, 8):
                    p, sh, fl, fn_ = struct.unpack_from("<QiIQ", blob, i)
                    if p == target:
                        print("  SSOEntry rva", hex(a+i), "sharedThread", sh, "flags", fl, "fn", hex(fn_))
