"""Search StarCitizen.exe for ASCII strings matching regexes. Usage: strings.py <regex> [<regex>...]
Caches the full string list (offset, text) the first time."""
import os, re, sys, pickle

EXE = r"C:\Program Files\Roberts Space Industries\StarCitizen\LIVE\Bin64\StarCitizen.exe"
CACHE = os.path.join(os.path.dirname(__file__), "strings.pkl")

def load():
    st = os.stat(EXE)
    if os.path.exists(CACHE):
        with open(CACHE, "rb") as f:
            key, items = pickle.load(f)
        if key == (st.st_size, st.st_mtime):
            return items
    data = open(EXE, "rb").read()
    items = [(m.start(), m.group().decode("ascii")) for m in re.finditer(rb"[\x20-\x7e]{5,}", data)]
    with open(CACHE, "wb") as f:
        pickle.dump(((st.st_size, st.st_mtime), items), f)
    return items

items = load()
for pat in sys.argv[1:]:
    rx = re.compile(pat, re.I)
    hits = [(o, s) for o, s in items if rx.search(s)]
    print(f"== {pat}: {len(hits)} hits")
    for o, s in hits[:80]:
        print(f"  0x{o:08x}  {s[:200]}")
