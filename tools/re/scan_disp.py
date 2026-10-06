"""scan_disp.py <disp hex> <filter regex> [start end]: find every instruction in .text with [reg + disp32]
by searching the displacement bytes and decoding candidate starts. Prints matches of the filter."""
import sys, re, struct
from pe import PE

pe = PE()
disp = int(sys.argv[1], 16)
flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
name, va, vsz, raw, rsz = pe.sec(".text")
lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else va
hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else va + rsz
text = pe.data[raw:raw + rsz]
pat = struct.pack("<I", disp)
needle = f"+ 0x{disp:x}]"
seen = set()
p = text.find(pat, lo - va)
while p >= 0 and va + p < hi:
    for k in range(2, 8):
        s = p - k
        ins = list(pe.md.disasm(text[s:s + 15], pe.base + va + s, count=1))
        if not ins: continue
        i = ins[0]
        if needle in i.op_str and s + i.size >= p + 4 and (va + s) not in seen:
            line = f"{va + s:08x}  {i.mnemonic:6} {i.op_str}"
            if not flt or flt.search(line):
                print(line)
            seen.add(va + s)
            break
    p = text.find(pat, p + 1)
