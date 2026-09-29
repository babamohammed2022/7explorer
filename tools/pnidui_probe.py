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
h = k32.LoadLibraryExW(p, None, 0x8)
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
obj = ctypes.c_void_p()
hr = ci(cf, None, ctypes.byref(iid_ct), ctypes.byref(obj))
print("CreateInstance(IOleCommandTarget) hr", hex(hr & 0xffffffff))
u32 = ctypes.WinDLL('user32')
for rid in (1, 2, 3, 100, 200, 1000):
    buf = ctypes.create_unicode_buffer(256)
    n = u32.LoadStringW(W.HMODULE(h), rid, buf, 256)
    print("LoadString", rid, n)
