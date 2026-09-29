"""CI-only runtime check of the Windows 8.1 stobject.dll/batmeter.dll on the
current Windows build (GitHub runner = Windows Server 2025 / 24H2 base).

1. every import (static + delay) resolvable on this OS?
2. MUI resource of the 8.1 files vs the current system .mui (checksum)
3. load from a private folder, LoadString counts, UIFILE present,
   DllGetClassObject(CLSID_SysTray) -> IClassFactory -> CreateInstance
Nothing here is committed except the text report.
"""
import ctypes, ctypes.wintypes as wt, os, shutil, struct, sys, uuid
import pefile

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.LoadLibraryExW.restype = wt.HMODULE
k32.LoadLibraryExW.argtypes = [wt.LPCWSTR, wt.HANDLE, wt.DWORD]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_void_p]
k32.FindResourceW.restype = ctypes.c_void_p
k32.FindResourceW.argtypes = [wt.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
k32.GetModuleFileNameW.argtypes = [wt.HMODULE, wt.LPWSTR, wt.DWORD]
u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.LoadStringW.argtypes = [wt.HMODULE, wt.UINT, wt.LPWSTR, ctypes.c_int]
LOAD_LIBRARY_AS_DATAFILE = 2
LOAD_WITH_ALTERED_SEARCH_PATH = 8

def check_imports(path):
    pe = pefile.PE(path)
    missing = []
    for attr in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT"):
        for d in getattr(pe, attr, []):
            dll = d.dll.decode()
            if dll.lower() == "batmeter.dll":
                continue  # provided by the 8.1 package itself
            h = k32.LoadLibraryExW(dll, None, 0)
            if not h:
                missing.append(f"{attr[16:]} {dll}: MODULE MISSING (err {ctypes.get_last_error()})")
                continue
            for i in d.imports:
                arg = ctypes.c_void_p(i.ordinal) if i.name is None else ctypes.c_char_p(i.name)
                if not k32.GetProcAddress(h, arg):
                    missing.append(f"{attr[16:]} {dll}!{i.name.decode() if i.name else '#%d' % i.ordinal}")
    print(f"--- unresolved imports of {os.path.basename(path)}: {len(missing)}")
    for m in missing: print("  ", m)

def mui_res(path):
    try:
        pe = pefile.PE(path)
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if t.name and str(t.name) == "MUI":
                e = t.directory.entries[0].directory.entries[0]
                return pe.get_data(e.data.struct.OffsetToData, e.data.struct.Size)
    except Exception as ex:
        return f"ERR {ex}".encode()
    return None

def main(stobject, batmeter):
    for p in (stobject, batmeter):
        check_imports(p)
    lang = "en-US"
    for name, p in (("stobject.dll", stobject), ("batmeter.dll", batmeter)):
        a = mui_res(p)
        b = mui_res(fr"C:\Windows\System32\{lang}\{name}.mui")
        print(f"--- MUI resource {name}")
        print("  8.1 LN :", a[:0x60].hex() if a else None)
        print("  cur mui:", b[:0x60].hex() if b else None)
    cache = os.path.abspath("work/cache")
    os.makedirs(os.path.join(cache, lang), exist_ok=True)
    shutil.copy(batmeter, os.path.join(cache, "batmeter.dll"))
    shutil.copy(stobject, os.path.join(cache, "stobject.dll"))
    for variant in ("no-mui", "cur-mui"):
        if variant == "cur-mui":
            for n in ("stobject.dll", "batmeter.dll"):
                src = fr"C:\Windows\System32\{lang}\{n}.mui"
                if os.path.exists(src): shutil.copy(src, os.path.join(cache, lang, n + ".mui"))
            print("  (must run in a fresh process for MUI change; see second invocation)")
            continue
    hb = k32.LoadLibraryExW(os.path.join(cache, "batmeter.dll"), None, LOAD_WITH_ALTERED_SEARCH_PATH)
    print("--- load batmeter:", hex(hb or 0), ctypes.get_last_error())
    hs = k32.LoadLibraryExW(os.path.join(cache, "stobject.dll"), None, LOAD_WITH_ALTERED_SEARCH_PATH)
    print("--- load stobject:", hex(hs or 0), ctypes.get_last_error())
    if not hs: return
    buf = ctypes.create_unicode_buffer(512)
    k32.GetModuleFileNameW(hs, buf, 512); print("  stobject path", buf.value)
    # which batmeter did stobject bind to?
    hbm = k32.LoadLibraryExW("batmeter.dll", None, 0)
    k32.GetModuleFileNameW(hbm, buf, 512); print("  batmeter bound:", buf.value)
    got = [(i, None) for i in range(0, 0)]
    ok = 0; sample = []
    for i in range(1, 40000):
        n = u32.LoadStringW(hs, i, buf, 512)
        if n > 0:
            ok += 1
            if len(sample) < 25: sample.append((i, buf.value[:70]))
    print(f"  stobject LoadString hits: {ok}"); [print("   ", s) for s in sample]
    ok = 0
    for i in range(1, 40000):
        if hb and u32.LoadStringW(hb, i, buf, 512) > 0: ok += 1
    print(f"  batmeter LoadString hits: {ok}")
    print("  UIFILE 500:", bool(k32.FindResourceW(hs, ctypes.c_void_p(500), ctypes.c_wchar_p("UIFILE"))))
    ole = ctypes.OleDLL("ole32"); ole.CoInitializeEx(None, 2)
    dgco = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(
        k32.GetProcAddress(hs, b"DllGetClassObject"))
    clsid = uuid.UUID("35CEC8A3-2BE6-11D2-8773-92E220524153").bytes_le
    iid_cf = uuid.UUID("00000001-0000-0000-C000-000000000046").bytes_le
    cf = ctypes.c_void_p()
    hr = dgco(clsid, iid_cf, ctypes.byref(cf))
    print(f"  DllGetClassObject(CLSID_SysTray) hr=0x{hr & 0xffffffff:08x}")
    if hr == 0 and cf.value:
        vt = ctypes.cast(ctypes.cast(cf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ci = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))(vt[3])
        iid_oct = uuid.UUID("B722BCCB-4E68-101B-A2BC-00AA00404770").bytes_le
        obj = ctypes.c_void_p()
        hr = ci(cf, None, iid_oct, ctypes.byref(obj))
        print(f"  CreateInstance(IOleCommandTarget) hr=0x{hr & 0xffffffff:08x} obj={obj.value}")

if __name__ == "__main__":
    if sys.argv[1] == "--strings-only":
        hs = k32.LoadLibraryExW(os.path.abspath(sys.argv[2]), None, LOAD_WITH_ALTERED_SEARCH_PATH)
        buf = ctypes.create_unicode_buffer(512)
        ok = sum(1 for i in range(1, 40000) if u32.LoadStringW(hs, i, buf, 512) > 0)
        print(f"--- {sys.argv[2]} with cur-mui copied: load={hex(hs or 0)} LoadString hits={ok}")
    else:
        main(sys.argv[1], sys.argv[2])
