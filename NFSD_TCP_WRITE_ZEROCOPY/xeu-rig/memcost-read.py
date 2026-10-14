#!/usr/bin/env python3
# memcost-read.py <prefix> <port> [<GB/s that counts as a cell: 4>]
#
# From what memcost-log.sh took: each stretch in which the port carried
# load (a cell), and over its steady part what was asked of memory for
# each 64-byte line of payload.  Payload is the port's bytes less each
# frame's headers (7740 of 7806 at the rig's MTU of 7792).
import statistics as st, sys

P, port = sys.argv[1], sys.argv[2]
floor = float(sys.argv[3]) if len(sys.argv) > 3 else 4
t0 = float(open(P + '.t0').read()); ev = {}; step = None
for l in open(P + '.cmn.txt'):
    p = l.strip().split(',')
    try:
        t = float(p[0]); v = float(p[1])
    except (ValueError, IndexError):
        continue
    n = p[3].split('/')[1] if '/' in p[3] else p[3]
    ev.setdefault(round(t, 1), {})[n] = v
    ev[round(t, 1)]['pct_' + n] = p[5]
ts = sorted(ev); step = ts[1] - ts[0] if len(ts) > 1 else 2.0
rx = []
for l in open(P + '.srv.txt'):
    p = l.split()
    d = dict(kv.split('=', 1) for kv in p[1:] if '=' in kv)
    rx.append((float(p[0]), int(d[port + '.rx_bytes'])))
drv = [tuple(float(x) for x in l.split()) for l in open(P + '.drv.txt') if len(l.split()) == 3]
near = lambda s, t, i=1: min(s, key=lambda r: abs(r[0] - t))[i]
rows = []
for t in ts:
    a, b = t0 + t - step, t0 + t
    rows.append((t, (near(rx, b) - near(rx, a)) / step / 1e9, (near(drv, b) - near(drv, a)) * 512 / step / 1e9, near(drv, b, 2), ev[t]))
cells, cur = [], []
for r in rows + [(0, 0, 0, 0, {})]:
    if r[1] > floor:
        cur.append(r)
    else:
        if len(cur) * step >= 20:
            cells.append(cur)
        cur = []
print('the mesh counted %s%% of the time' % ev[ts[len(ts) // 2]].get('pct_hnf_mc_reqs', '?'))
for c in cells:
    c = c[int(8 / step):-int(4 / step)]	# the steady part: 8 s in, 4 s from the end
    if not c:
        continue
    L = [r[1] * 1e9 / 64 * 7740 / 7806 for r in c]
    per = lambda k: [r[4][k] / step / l for r, l in zip(c, L) if k in r[4]]
    m = per('hnf_mc_reqs')
    print('%4d to %4d s: wire %.1f GB/s (%.1f to %.1f), drives writing %.1f GB/s, region in use %d pages (%.0f MiB) at the middle, %d at most;'
          ' requests to memory %.2fe9/s = %.2f a line of payload (%.2f to %.2f); cache fills %.2f a line; lmiss %.2f, bus_wr %.2f' % (
          c[0][0], c[-1][0], st.median(r[1] for r in c), min(r[1] for r in c), max(r[1] for r in c),
          st.median(r[2] for r in c), st.median(r[3] for r in c), st.median(r[3] for r in c) * 16 / 1024, max(r[3] for r in c),
          st.median(r[4]['hnf_mc_reqs'] / step for r in c) / 1e9, st.median(m), min(m), max(m),
          st.median(per('hnf_cache_fill')), st.median(per('l3d_cache_lmiss_rd')), st.median(per('bus_access_wr'))))
