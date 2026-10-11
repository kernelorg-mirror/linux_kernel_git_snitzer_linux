#!/usr/bin/python3
# rss-place.py <flows> <rings> <table entries> [shift] - place the flows a
# port is receiving on its rings, as evenly as their bytes allow.
#
# <flows> is bpftrace's print of a map @flow[hash, napi id] = bytes, taken
# from net:napi_gro_receive_entry under the load to be placed (xeu-rx.bt has
# one).  On xeu the hash is the card's, and bits <shift> (13) and up pick the
# flow's entry of the RSS table, so pointing that entry at a ring puts the
# flow there.  Flows that share an entry go together.  The entry with the
# most flows (then the most bytes) is given to the ring with the fewest
# flows so far (then the fewest bytes), and so on down; entries no flow
# uses keep the even spread.  Flows are counted before bytes because a few
# seconds' sample of like writers does not show them carrying the same,
# and balancing its bytes put 5 and 3 flows on two rings of four.
#
# stdout: the ethtool netlink rss-set request's "indir" array, as JSON.
# stderr: a line per flow (hash, entry, ring, bytes) and each ring's flows
# and share, for the cell's record.
import json, re, sys

flows, nr, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
shift = int(sys.argv[4]) if len(sys.argv) > 4 else 13
fl = {}
for l in open(flows):
    m = re.match(r'@flow\[(\d+), (\d+)\]: (\d+)', l)
    if m:
        fl[int(m[1])] = fl.get(int(m[1]), 0) + int(m[3])
if not fl:
    sys.exit("no flows in " + flows)
# A flow worth placing carries data: ignore what took under 5% of the busiest.
top = max(fl.values())
fl = {h: b for h, b in fl.items() if b * 20 >= top}
ent = {}
for h, b in fl.items():
    ent.setdefault((h >> shift) % n, []).append(h)
load = [0] * nr
count = [0] * nr
tbl = [i % nr for i in range(n)]
for e in sorted(ent, key=lambda e: (-len(ent[e]), -sum(fl[h] for h in ent[e]))):
    r = min(range(nr), key=lambda r: (count[r], load[r], r))
    tbl[e] = r
    load[r] += sum(fl[h] for h in ent[e])
    count[r] += len(ent[e])
tot = sum(load)
for e in sorted(ent):
    for h in ent[e]:
        sys.stderr.write("flow %#010x entry %3d ring %d bytes %d%s\n" %
                         (h, e, tbl[e], fl[h], " (shares its entry)" if len(ent[e]) > 1 else ""))
sys.stderr.write("placed %d flows on %d entries: flows a ring %s, expected share %s%%\n" %
                 (len(fl), len(ent), "/".join(map(str, count)),
                  "/".join(str(round(100 * x / tot)) for x in load)))
print(json.dumps(tbl))
