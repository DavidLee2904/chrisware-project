"""func.py <rva> [maxlen]: summarize a function: string refs and calls (robust sweep that skips bad bytes),
plus direct callers of the function."""
import sys, re, struct
from pe import PE

pe = PE()
start = int(sys.argv[1], 16)
maxlen = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x1000
code = pe.read(start, maxlen)
off = 0
strs, calls = [], []
while off < len(code):
    ins = list(pe.md.disasm(code[off:off + 15], pe.base + start + off, count=1))
    if not ins:
        off += 1; continue
    i = ins[0]
    if i.mnemonic == "int3" and code[off:off + 2] == b"\xcc\xcc": break
    m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", i.op_str)
    if m:
        t = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == "+" else -1) - pe.base
        try:
            s = pe.cstr(t, 160)
            if len(s) >= 4 and s.isprintable(): strs.append((i.address - pe.base, s[:150]))
        except Exception: pass
    if i.mnemonic == "call" and re.match(r"^0x[0-9a-f]+$", i.op_str):
        calls.append((i.address - pe.base, int(i.op_str, 16) - pe.base))
    off += i.size
print(f"function {start:08x}, ~{off:#x} bytes")
for a, s in strs: print(f"  str  {a:08x}  \"{s}\"")
print("  calls:", " ".join(f"{t:08x}" for _, t in calls[:60]))
name, va, vsz, raw, rsz = pe.sec(".text")
text = pe.data[raw:raw + rsz]
callers = []
p = text.find(b"\xe8")
for m in re.finditer(rb"\xe8", text):
    q = m.start()
    if va + q + 5 + struct.unpack_from("<i", text, q + 1)[0] == start:
        callers.append(va + q)
for m in re.finditer(rb"\xe9", text):
    q = m.start()
    if va + q + 5 + struct.unpack_from("<i", text, q + 1)[0] == start:
        callers.append(va + q)
print("  callers:", " ".join(f"{c:08x}(f~{pe.func_start(c) or 0:08x})" for c in callers[:30]))
