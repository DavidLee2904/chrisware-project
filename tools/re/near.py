"""Print the strings stored near a file offset. Usage: near.py <hexoffset> [before] [after]"""
import os, pickle, sys, bisect
CACHE = os.path.join(os.path.dirname(__file__), "strings.pkl")
items = pickle.load(open(CACHE, "rb"))[1]
offs = [o for o, _ in items]
for arg in sys.argv[1:]:
    parts = arg.split(":")
    off = int(parts[0], 16)
    before = int(parts[1]) if len(parts) > 1 else 8
    after = int(parts[2]) if len(parts) > 2 else 8
    i = bisect.bisect_left(offs, off)
    print(f"== near 0x{off:08x}")
    for o, s in items[max(0, i - before): i + after]:
        print(f"  0x{o:08x}{' >' if o == off else '  '} {s[:400]}")
