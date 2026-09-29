"""Resolve RVAs of Win7 explorer.exe with the public PDB and disassemble
wWinMain (return paths). Diagnostic only; usage: explorer_sym.py exe sym_dir dbghelp rva..."""
import sys, ctypes, ctypes.wintypes as W, pefile, capstone
exe, symdir, dbghelp_path = sys.argv[1:4]
rvas = [int(x, 16) for x in sys.argv[4:]]
dh = ctypes.WinDLL(dbghelp_path)
proc = W.HANDLE(0x1234)
dh.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME|DEFERRED|LOAD_LINES
assert dh.SymInitializeW(proc, ctypes.c_wchar_p(symdir), False)
BASE = 0x140000000
pe = pefile.PE(exe)
size = pe.OPTIONAL_HEADER.SizeOfImage
dh.SymLoadModuleExW.restype = ctypes.c_uint64
b = dh.SymLoadModuleExW(proc, None, ctypes.c_wchar_p(exe), None, ctypes.c_uint64(BASE), size, None, 0)
print("module base", hex(b))
class SI(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", W.ULONG),
                ("Scope", W.ULONG), ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
                ("Name", ctypes.c_wchar * 512)]
def sym(addr):
    s = SI(); s.SizeOfStruct = ctypes.sizeof(SI) - 512 * 2 + 2; s.MaxNameLen = 511
    d = ctypes.c_uint64(0)
    if dh.SymFromAddrW(proc, ctypes.c_uint64(addr), ctypes.byref(d), ctypes.byref(s)):
        return s.Name, d.value, s.Address, s.Size
    return None, 0, 0, 0
imports = {}
for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    for i in e.imports:
        imports[i.address - pe.OPTIONAL_HEADER.ImageBase + BASE] = "%s!%s" % (e.dll.decode(), (i.name or b"#%d" % i.ordinal).decode())
for r in rvas:
    n, d, a, sz = sym(BASE + r)
    print("RVA 0x%X -> %s+0x%X" % (r, n, d))
def disasm_func(name_or_rva, maxlen=0x1800):
    s = SI(); s.SizeOfStruct = ctypes.sizeof(SI) - 512 * 2 + 2; s.MaxNameLen = 511
    if isinstance(name_or_rva, str):
        if not dh.SymFromNameW(proc, ctypes.c_wchar_p(name_or_rva), ctypes.byref(s)):
            print("no symbol", name_or_rva); return
        addr, sz = s.Address, s.Size or maxlen
    else:
        n, d, addr, sz = sym(BASE + name_or_rva); sz = sz or maxlen
    print("\n==== %s at RVA 0x%X size 0x%X" % (name_or_rva, addr - BASE, sz))
    rva = addr - BASE
    data = pe.get_data(rva, sz)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for ins in md.disasm(data, addr):
        note = ""
        if ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j") or "rip" in ins.op_str:
            tgt = None
            if ins.op_str.startswith("0x"):
                tgt = int(ins.op_str, 16)
            elif "rip +" in ins.op_str or "rip -" in ins.op_str:
                import re
                m = re.search(r"rip ([+-]) (0x[0-9a-f]+)", ins.op_str)
                if m:
                    off = int(m.group(2), 16); tgt = ins.address + ins.size + (off if m.group(1) == "+" else -off)
            if tgt:
                if tgt in imports: note = imports[tgt]
                else:
                    n, d, a, z = sym(tgt)
                    if n and not (ins.mnemonic.startswith("j") and a == addr): note = "%s+0x%X" % (n, d)
        print("  %X: %-8s %-40s %s" % (ins.address - BASE, ins.mnemonic, ins.op_str, note))
for f in ["wWinMain", "WinMain", "ExplorerWinMain", "_wWinMainCRTStartup"]:
    disasm_func(f)
