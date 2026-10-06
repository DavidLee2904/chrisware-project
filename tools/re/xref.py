"""xref.py <string> [before] [after]: find code referencing a string and disassemble around it."""
import sys
from pe import PE, fmt

pe = PE()
s = sys.argv[1]
before = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x60
after = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x80
t = pe.find_string_rva(s)
print(f"string '{s}' at rva {t:08x}" if t else f"string '{s}' not found")
if t:
    for x in pe.xrefs(t):
        print(f"--- xref at {x:08x} (func start ~ {pe.func_start(x) or 0:08x})")
        start = x - before
        ins = pe.dis(start, before + after)
        # resync: drop leading instructions until we hit the xref address boundary
        ok = [i for i in ins]
        print(fmt(ok, pe, mark=x))
