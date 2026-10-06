"""cryxml.py <file>: print a CryXmlB (binary XML) file as text XML. Plain-text XML is printed as is."""
import struct, sys
from xml.sax.saxutils import quoteattr, escape

def decode(d):
    if not d.startswith(b"CryXmlB"): return d.decode("utf-8", "replace")
    (_, ntab, ncount, atab, acount, ctab, ccount, stab, ssize) = struct.unpack_from("<9I", d, 8)
    s = lambda o: d[stab + o: d.index(b"\0", stab + o)].decode("utf-8", "replace")
    nodes = [struct.unpack_from("<IIHHiII", d, ntab + i * 28) for i in range(ncount)]
    attrs = [struct.unpack_from("<II", d, atab + i * 8) for i in range(acount)]
    kids = struct.unpack_from(f"<{ccount}I", d, ctab)
    out = []
    def emit(i, depth):
        tag, content, na, nc, parent, fa, fc = nodes[i]
        a = "".join(f" {s(k)}={quoteattr(s(v))}" for k, v in attrs[fa:fa + na])
        text = s(content)
        if not nc and not text: out.append(f"{'  ' * depth}<{s(tag)}{a}/>"); return
        out.append(f"{'  ' * depth}<{s(tag)}{a}>{escape(text)}")
        for c in kids[fc:fc + nc]: emit(c, depth + 1)
        out.append(f"{'  ' * depth}</{s(tag)}>")
    emit(0, 0)
    return "\n".join(out)

if __name__ == "__main__":
    print(decode(open(sys.argv[1], "rb").read()))
