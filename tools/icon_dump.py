"""CI: dump every RT_GROUP_ICON of a PE as PNG (largest <=32px frame) for review."""
import sys, os, struct, io
import pefile
from PIL import Image
pe = pefile.PE(sys.argv[1]); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
icons = {}; groups = {}
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    for e in t.directory.entries:
        d = e.directory.entries[0].data.struct
        blob = pe.get_data(d.OffsetToData, d.Size)
        if t.id == 3: icons[e.id] = blob
        if t.id == 14: groups[e.id if e.id is not None else str(e.name)] = blob
for gid, g in groups.items():
    _, _, n = struct.unpack_from("<HHH", g)
    ents = [struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i) for i in range(n)]
    ents = [x for x in ents if x[7] in icons]
    pick = sorted(ents, key=lambda x: abs((x[0] or 256) - 32) * 10 - x[5])[0]
    data = icons[pick[7]]
    hdr = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", *pick[:6], len(data), 22)
    try:
        im = Image.open(io.BytesIO(hdr + data)); im.save(os.path.join(out, f"{gid}.png"))
        print(gid, im.size)
    except Exception as ex:
        print(gid, "fail", ex)
