#!/usr/bin/env python3
# stalled.py <drives-sec.tsv> <wire-sec.tsv> <client-sec.tsv> <warm> <runtime>:
# drives.txt's STALLED line.  Over every second of the load, the warm-up
# included (from the third, to the last before fio ends), a stall is a run of
# seconds in which all the drives together wrote under half their median
# second.  For each: the seconds, the least the drives wrote together and
# the wire carried in one of them, the client's retransmits over them, and
# the drives at 90% busy or more in its first second (where it started: a
# drive that stalls is busy, the ones it starves are not).  A stall the
# wire falls with started before the drives; one the wire holds through
# started after the socket.
#
# drives-sec.tsv and wire-sec.tsv are drvsample.py's and wiresample.py's,
# started together, row for row.  client-sec.tsv is a time and the client's
# Tcp RetransSegs, cumulative, each second.
import sys

def table(f):
    rows = [l.split() for l in open(f)]
    return rows[0], {int(r[0]): r for r in rows[1:] if r and r[0].isdigit()}

dh, drv = table(sys.argv[1])
wh, wire = table(sys.argv[2])
cli = []
for l in open(sys.argv[3]):
    f = l.split()
    if len(f) == 2 and f[1].isdigit():
        cli.append((float(f[0]), int(f[1])))
warm, runtime = int(sys.argv[4]), int(sys.argv[5])

gb = [i for i, k in enumerate(dh) if k.endswith('_GBps')]
wgb = [i for i, k in enumerate(wh) if k.endswith('_GBps')]
secs = [s for s in sorted(drv) if 3 <= s < warm + runtime]
tot = {s: sum(float(drv[s][i]) for i in gb) for s in secs}
if not tot:
    print('STALLED: no seconds sampled'); sys.exit()
v = sorted(tot.values()); med = v[len(v) // 2]

def retrans(a, b):
    # the client's retransmits from the start of second a to the end of b
    if a - 1 not in wire or b not in wire or not cli:
        return '-'
    t0, t1 = float(wire[a - 1][1]), float(wire[b][1])
    c0 = [c for t, c in cli if t <= t0 + 0.5]; c1 = [c for t, c in cli if t <= t1 + 0.5]
    return c1[-1] - c0[-1] if c0 and c1 else '-'

out, run = [], []
for s in secs + [None]:
    if s is not None and tot[s] < med / 2:
        run.append(s); continue
    if run:
        a, b = run[0], run[-1]
        w = [sum(float(wire[t][i]) for i in wgb) for t in run if t in wire]
        busy = [dh[i].split('_')[0] for i in gb if float(drv[a][i + 1]) >= 90]
        out.append('%d to %d s, the drives down to %.1f GB/s, the wire down to %s GB/s, client retransmits %s, busy at the start: %s'
                   % (a, b, min(tot[t] for t in run), '%.1f' % min(w) if w else '-', retrans(a, b), ' '.join(busy) or 'none'))
        run = []
print('STALLED: ' + '; '.join(out) if out else 'no stall (the drives together never under half their median of %.1f GB/s)' % med)
