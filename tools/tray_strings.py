"""Dump interesting UTF-16/ASCII strings of tray DLLs (diagnostic only)."""
import sys, re, pefile
KEYS = re.compile(r"(?i)(software\\|flyout|mtcuvc|van|immersive|pnidui|sndvol|stobject|ssoid|shellexperience|win32|\{[0-9a-f-]{36}\}|enable|disable|legacy|control ?center|actioncenter|quick)")
for f in sys.argv[1:]:
    data = open(f, "rb").read()
    print("=====", f, len(data))
    try:
        pe = pefile.PE(f)
        print("version", pe.FileInfo and [st.entries.get(b'FileVersion') for fi in pe.FileInfo for x in fi if hasattr(x,'StringTable') for st in x.StringTable])
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            print(" imp", e.dll.decode(), len(e.imports))
        for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
            print(" dimp", e.dll.decode())
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            print(" exports", [(s.ordinal, s.name) for s in pe.DIRECTORY_ENTRY_EXPORT.symbols][:40])
    except Exception as ex:
        print("pe error", ex)
    seen = set()
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", data):
        s = m.group().decode("utf-16le")
        if KEYS.search(s) and s not in seen:
            seen.add(s); print("  W 0x%x %s" % (m.start(), s))
    for m in re.finditer(rb"[\x20-\x7e]{6,}", data):
        s = m.group().decode()
        if KEYS.search(s) and s not in seen and len(s) < 200:
            seen.add(s); print("  A 0x%x %s" % (m.start(), s))
