"""Small PE helpers for StarCitizen.exe: section mapping, RIP-relative xref search, disassembly.
Loads the image once into memory (mapped by section)."""
import os, re, struct, mmap
import capstone

EXE = r"C:\Program Files\Roberts Space Industries\StarCitizen\LIVE\Bin64\StarCitizen.exe"

class PE:
    def __init__(self, path=EXE):
        self.f = open(path, "rb")
        self.data = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        self.secs = []
        so = pe + 24 + optsz
        for i in range(nsec):
            name = d[so:so + 8].rstrip(b"\0").decode()
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, so + 8)
            self.secs.append((name, va, vsz, raw, rsz))
            so += 40
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = False

    def sec(self, name):
        for s in self.secs:
            if s[0] == name: return s
        raise KeyError(name)

    def off2rva(self, off):
        for name, va, vsz, raw, rsz in self.secs:
            if raw <= off < raw + rsz: return va + off - raw
        return None

    def rva2off(self, rva):
        for name, va, vsz, raw, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz): return raw + rva - va
        return None

    def read(self, rva, n):
        o = self.rva2off(rva)
        return bytes(self.data[o:o + n])

    def cstr(self, rva, n=200):
        b = self.read(rva, n)
        return b.split(b"\0")[0].decode("latin1")

    def find_string_rva(self, s):
        o = self.data.find(s.encode() + b"\0")
        while o >= 0:
            if o == 0 or self.data[o - 1] == 0:
                return self.off2rva(o)
            o = self.data.find(s.encode() + b"\0", o + 1)
        return None

    def xrefs(self, target_rva):
        """All instructions with a RIP-relative disp32 pointing at target (lea/mov/cmp...)."""
        name, va, vsz, raw, rsz = self.sec(".text")
        text = self.data[raw:raw + rsz]
        hits = []
        # scan for any 4-byte disp such that insn_end + disp == target; check typical encodings
        for m in re.finditer(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text):
            p = m.start()
            disp = struct.unpack_from("<i", text, p + 3)[0]
            if va + p + 7 + disp == target_rva:
                hits.append(va + p)
        return hits

    def dis(self, rva, n=0x100):
        code = self.read(rva, n)
        return list(self.md.disasm(code, self.base + rva))

    def func_start(self, rva, maxback=0x4000):
        """Heuristic: walk back to the previous run of CC padding."""
        o = self.rva2off(rva)
        d = self.data
        for i in range(o, o - maxback, -1):
            if d[i - 1] == 0xCC and d[i - 2] == 0xCC:
                return self.off2rva(i)
        return None

def fmt(insns, pe, mark=None):
    out = []
    for i in insns:
        rva = i.address - pe.base
        s = f"  {rva:08x}{' >' if rva == mark else '  '} {i.mnemonic:6} {i.op_str}"
        # annotate rip-relative string refs
        m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", i.op_str)
        if m:
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            t = i.address + i.size + disp - pe.base
            try:
                cs = pe.cstr(t, 120)
                if len(cs) >= 4 and all(32 <= ord(c) < 127 for c in cs):
                    s += f'   ; "{cs}"'
                else:
                    s += f"   ; -> {t:08x}"
            except Exception:
                s += f"   ; -> {t:08x}"
        m = re.search(r"^0x([0-9a-f]+)$", i.op_str)
        if i.mnemonic in ("call", "jmp") and m:
            s += f"   ; sub_{int(m.group(1),16) - pe.base:08x}"
        out.append(s)
    return "\n".join(out)
