"""dis.py <rva> [len]: disassemble at an RVA. Also: dis.py xref <rva> lists code refs to that RVA (rip-relative)."""
import sys
from pe import PE, fmt
pe = PE()
if sys.argv[1] == "xref":
    t = int(sys.argv[2], 16)
    for x in pe.xrefs(t):
        print(f"{x:08x}  (func ~ {pe.func_start(x) or 0:08x})")
    # also direct calls (E8 rel32) to t
    import re, struct
    name, va, vsz, raw, rsz = pe.sec(".text")
    text = pe.data[raw:raw + rsz]
    n = 0
    for m in re.finditer(rb"\xe8", text):
        p = m.start()
        if va + p + 5 + struct.unpack_from("<i", text, p + 1)[0] == t:
            print(f"call from {va+p:08x}  (func ~ {pe.func_start(va+p) or 0:08x})"); n += 1
            if n > 40: break
else:
    rva = int(sys.argv[1], 16)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x200
    print(fmt(pe.dis(rva, n), pe))
