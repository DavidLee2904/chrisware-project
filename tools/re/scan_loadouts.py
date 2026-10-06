"""scan_loadouts.py [prefix] [regex]: decode loadout XMLs under a Data.p4k folder and report attributes
matching a regex (default: anything head / dna / chf / custom related)."""
import re
import sys
from p4k import P4K
from cryxml import decode

prefix = (sys.argv[1] if len(sys.argv) > 1 else r'Data\Scripts\Loadouts\Player' + '\\').lower()
rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'(?i)\b([a-z_]*(?:head|dna|chf|custom)[a-z_]*)\s*=\s*"([^"]*)"')
pk = P4K()
hits, n = {}, 0
for e in pk.entries():
    name = e[0]
    low = name.lower()
    if not low.startswith(prefix) or not low.endswith('.xml'):
        continue
    try:
        txt = decode(pk.read(e))
    except Exception:
        continue
    n += 1
    for m in rx.finditer(txt):
        hits.setdefault(m.group(1), set()).add((m.group(2)[:80], name.split('\\')[-1]))
print('files', n)
for k, v in sorted(hits.items()):
    print(k, len(v), sorted(v)[:4])
