#!/usr/bin/python3
# balance.py <xeu-rx.bt output> <rings>: an rss-set request that gives each
# flow seen (by its hash's table entry, bits 13 to 19) a ring in turn.
import re, json, sys
fl = []
for l in open(sys.argv[1]):
    m = re.match(r'@flow\[(\d+), (\d+)\]: (\d+)', l)
    if m and int(m[3]) > 1e6: fl.append(int(m[1]))
nr = int(sys.argv[2])
t = [i % nr for i in range(128)]
ents = sorted(set((h >> 13) & 127 for h in fl))
for n, e in enumerate(ents): t[e] = n % nr
print(json.dumps({"header": {"dev-name": "enp161s0f0"}, "indir": t}))
sys.stderr.write(f"{len(fl)} flows on {len(ents)} distinct entries: {ents}\n")
