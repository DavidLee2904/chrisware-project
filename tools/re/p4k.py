"""p4k.py: read files out of Star Citizen's Data.p4k (ZIP64, zstd = method 100). Python 3.14+ (stdlib zstd).
Usage: p4k.py list <regex>          list entry names matching regex
       p4k.py get <name> <outfile>  extract one entry (exact name)"""
import mmap, os, re, struct, sys
from compression import zstd

P4K = r"C:\Program Files\Roberts Space Industries\StarCitizen\LIVE\Data.p4k"

class P4K:
    def __init__(self, path=P4K):
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.m
        eocd = m.rfind(b"PK\x05\x06", max(0, len(m) - 0x10000))
        loc = m.rfind(b"PK\x06\x07", max(0, eocd - 0x100), eocd)
        eocd64 = struct.unpack_from("<Q", m, loc + 8)[0]
        self.count, _, self.cd_off = struct.unpack_from("<QQQ", m, eocd64 + 32)

    def entries(self):
        m, p = self.m, self.cd_off
        for _ in range(self.count):
            if m[p:p + 4] != b"PK\x01\x02": break
            method, = struct.unpack_from("<H", m, p + 10)
            csize, usize = struct.unpack_from("<II", m, p + 20)
            nlen, xlen, clen = struct.unpack_from("<HHH", m, p + 28)
            lho, = struct.unpack_from("<I", m, p + 42)
            name = m[p + 46:p + 46 + nlen].decode("utf-8", "replace")
            x = p + 46 + nlen; xe = x + xlen
            vals = [usize, csize, lho]   # zip64 extra replaces 0xFFFFFFFF fields in this order
            while x + 4 <= xe:
                tag, sz = struct.unpack_from("<HH", m, x)
                if tag == 1:
                    q = x + 4
                    for i, v in enumerate(vals):
                        if v == 0xFFFFFFFF: vals[i] = struct.unpack_from("<Q", m, q)[0]; q += 8
                x += 4 + sz
            yield name, method, vals[1], vals[0], vals[2]
            p = xe + clen

    def read(self, entry):
        name, method, csize, usize, lho = entry
        m = self.m
        nlen, xlen = struct.unpack_from("<HH", m, lho + 26)
        data = m[lho + 30 + nlen + xlen: lho + 30 + nlen + xlen + csize]
        if method == 0: return bytes(data)
        if method == 100: return zstd.decompress(data)
        raise ValueError(f"method {method}")

if __name__ == "__main__":
    pk = P4K()
    if sys.argv[1] == "list":
        rx = re.compile(sys.argv[2], re.I)
        for e in pk.entries():
            if rx.search(e[0]): print(e[0], e[1], e[3])
    elif sys.argv[1] == "get":
        for e in pk.entries():
            if e[0] == sys.argv[2]:
                open(sys.argv[3], "wb").write(pk.read(e)); print("wrote", sys.argv[3]); break
        else: print("not found")
