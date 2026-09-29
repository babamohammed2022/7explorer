"""Diagnostic: structure of the Windows 8.1 stobject.dll / batmeter.dll.

Runs in CI only (the binaries are never committed). Prints imports, exports,
resources, embedded GUIDs that look like CLSID/IID, interesting strings and
disassembly of DllGetClassObject so we can see which class hosts the
battery flyout.
"""
import re, sys, struct, uuid
import pefile

KNOWN = {
    "35cec8a3-2be6-11d2-8773-92e220524153": "CLSID_SysTray (stobject)",
    "7007acc7-3202-11d1-aad2-00805fc1270e": "Network Connections",
    "00000001-0000-0000-c000-000000000046": "IID_IClassFactory",
    "b722bccb-4e68-101b-a2bc-00aa00404770": "IID_IOleCommandTarget",
    "fc4801a3-2ba9-11cf-a229-00aa003d7352": "IID_IObjectWithSite",
    "00000000-0000-0000-c000-000000000046": "IID_IUnknown",
    "000214e6-0000-0000-c000-000000000046": "IID_IShellFolder",
}

def guids(data):
    seen = {}
    for m in re.finditer(rb"(?s).{16}", b""):
        pass
    for off in range(0, len(data) - 16, 4):
        b = data[off:off + 16]
        d1, d2, d3 = struct.unpack_from("<IHH", b)
        if d1 == 0 or b[8:16] == b"\0" * 8 or b.count(0) > 6:
            continue
        # variant bits RFC4122 or MS (0x8x..0xBx / 0xCx)
        if not (0x80 <= b[8] <= 0xDF):
            continue
        g = str(uuid.UUID(bytes_le=b))
        seen.setdefault(g, off)
    return seen

def main(path):
    pe = pefile.PE(path)
    print("=" * 70); print(path)
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    print(f"TimeDateStamp=0x{fh.TimeDateStamp:08X} SizeOfImage=0x{oh.SizeOfImage:x} Machine=0x{fh.Machine:x}")
    print("--- exports")
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            print(f"  {e.ordinal:4} {e.name.decode() if e.name else '-'} rva=0x{e.address:x}"
                  + (f" -> {e.forwarder.decode()}" if e.forwarder else ""))
    for attr, title in (("DIRECTORY_ENTRY_IMPORT", "imports"), ("DIRECTORY_ENTRY_DELAY_IMPORT", "delay imports")):
        print(f"--- {title}")
        for d in getattr(pe, attr, []):
            names = [(i.name.decode() if i.name else f"#{i.ordinal}") for i in d.imports]
            print(f"  {d.dll.decode()}: {', '.join(names)}")
    print("--- resources")
    if hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            tn = str(t.name) if t.name else pefile.RESOURCE_TYPE.get(t.id, t.id)
            ids = [str(e.name) if e.name else str(e.id) for e in t.directory.entries]
            print(f"  {tn}: {len(ids)} -> {', '.join(ids[:40])}")
    data = open(path, "rb").read()
    print("--- guid-like constants (first 80, known ones labelled)")
    gs = guids(data)
    for i, (g, off) in enumerate(sorted(gs.items(), key=lambda x: x[1])):
        if i >= 400: break
        lab = KNOWN.get(g, "")
        try:
            rva = pe.get_rva_from_offset(off)
        except Exception:
            rva = 0
        sec = next((s.Name.rstrip(b"\0").decode() for s in pe.sections
                    if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize), "?")
        if sec in (".rdata", ".data") or lab:
            print(f"  {g} rva=0x{rva:x} {sec} {lab}")
    print("--- interesting strings")
    pat = re.compile(r"flyout|battery|batmeter|power|clsid|window|class|\.dll|immersive|flyout", re.I)
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", data):
        s = m.group().decode("utf-16le")
        if pat.search(s): print("  W", hex(m.start()), s[:160])
    for m in re.finditer(rb"[\x20-\x7e]{6,}", data):
        s = m.group().decode()
        if pat.search(s) and not s.startswith("api-ms"): print("  A", hex(m.start()), s[:160])
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        for e in getattr(pe, "DIRECTORY_ENTRY_EXPORT", pefile.Structure({})).symbols:
            if e.name and e.name.decode() in ("DllGetClassObject", "DllCanUnloadNow"):
                code = pe.get_data(e.address, 0x200)
                print(f"--- disasm {e.name.decode()}")
                for ins in md.disasm(code, oh.ImageBase + e.address):
                    print(f"  {ins.address:x} {ins.mnemonic} {ins.op_str}")
                    if ins.mnemonic == "ret": break
    except Exception as ex:
        print("capstone:", ex)

for p in sys.argv[1:]:
    try:
        main(p)
    except Exception as ex:
        print("ERROR", p, ex)
