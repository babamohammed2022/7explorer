"""CI-only: can the current system .mui be re-keyed for the 8.1 LN file?
Patches the MUI checksums of C:\\Windows\\System32\\<lang>\\X.dll.mui to the
values in the 8.1 LN file, then (in a fresh process) loads the LN and checks
that the string ids referenced by the flyout UIFILE resolve."""
import ctypes, ctypes.wintypes as wt, os, re, shutil, subprocess, sys
import pefile

def mui_entry(pe):
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.name and str(t.name) == "MUI":
            e = t.directory.entries[0].directory.entries[0]
            return e.data.struct.OffsetToData, e.data.struct.Size
    raise RuntimeError("no MUI resource")

def patch(ln_path, mui_src, mui_dst):
    ln = pefile.PE(ln_path); rva, size = mui_entry(ln)
    lnd = ln.get_data(rva, size)
    mp = pefile.PE(mui_src); mrva, msize = mui_entry(mp)
    off = mp.get_offset_from_rva(mrva)
    data = bytearray(open(mui_src, "rb").read())
    data[off + 0x1C: off + 0x3C] = lnd[0x1C:0x3C]
    os.makedirs(os.path.dirname(mui_dst), exist_ok=True)
    open(mui_dst, "wb").write(data)

def child(ln):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.LoadLibraryExW.restype = wt.HMODULE
    k32.LoadLibraryExW.argtypes = [wt.LPCWSTR, wt.HANDLE, wt.DWORD]
    u32 = ctypes.WinDLL("user32")
    u32.LoadStringW.argtypes = [wt.HMODULE, wt.UINT, wt.LPWSTR, ctypes.c_int]
    h = k32.LoadLibraryExW(os.path.abspath(ln), None, 8)
    buf = ctypes.create_unicode_buffer(1024)
    hits = {i: buf.value for i in range(1, 40000) if u32.LoadStringW(h, i, buf, 1024) > 0}
    print(f"  {ln}: LoadString hits={len(hits)}")
    pe = pefile.PE(ln)
    xml = None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.name and str(t.name) == "UIFILE":
            e = t.directory.entries[0].directory.entries[0]
            raw = pe.get_data(e.data.struct.OffsetToData, e.data.struct.Size)
            xml = raw.decode("utf-16le", "ignore") if raw[1:2] == b"\0" else raw.decode("latin1")
    if xml:
        ids = sorted({int(x) for x in re.findall(r"rcstr\((\d+)\)", xml)})
        miss = [i for i in ids if i not in hits]
        print(f"  UIFILE rcstr ids={len(ids)} missing={miss}")
        for i in ids[:40]:
            print(f"    {i}: {hits.get(i, '<MISSING>')[:80]}")
        icons = sorted(set(re.findall(r"rc\w+\((\d+)", xml)))
        print("  other rc refs:", icons[:40])
    # strings stobject loads by id at runtime (LoadStringW call sites unknown): list a sample
    print("  sample:", list(hits.items())[:30])

if __name__ == "__main__":
    if sys.argv[1] == "--child":
        child(sys.argv[2]); sys.exit()
    lang = sys.argv[1]
    for name in ("stobject.dll", "batmeter.dll"):
        ln = f"work/mui-test/{name}"
        os.makedirs("work/mui-test", exist_ok=True)
        shutil.copy(f"work/w81-{name}", ln)
        src = fr"C:\Windows\System32\{lang}\{name}.mui"
        print(f"--- {name}: source {src} exists={os.path.exists(src)}")
        if os.path.exists(src):
            patch(ln, src, f"work/mui-test/{lang}/{name}.mui")
            subprocess.run([sys.executable, __file__, "--child", ln])
