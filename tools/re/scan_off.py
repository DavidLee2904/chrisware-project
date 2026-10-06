"""scan_off.py <start_rva> <end_rva> <displacement>: list instructions using [reg + disp] in a range,
with the nearest preceding function-name string reference as context."""
import sys, re
from pe import PE

pe = PE()
start, end, disp = int(sys.argv[1], 16), int(sys.argv[2], 16), int(sys.argv[3], 16)
needle = f"+ 0x{disp:x}]"
code = pe.read(start, end - start)
last_fn = ""
for i in pe.md.disasm(code, pe.base + start):
    m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", i.op_str)
    if m:
        t = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1) - pe.base
        try:
            s = pe.cstr(t, 160)
            if "::" in s and len(s) > 8 and s.isprintable(): last_fn = s[:110]
        except Exception:
            pass
    if needle in i.op_str and "rsp" not in i.op_str and "rbp" not in i.op_str:
        print(f"{i.address - pe.base:08x}  {i.mnemonic:6} {i.op_str:45}  | {last_fn}")
