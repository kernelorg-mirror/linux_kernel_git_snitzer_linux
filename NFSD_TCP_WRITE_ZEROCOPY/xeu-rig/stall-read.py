#!/usr/bin/env python3
# stall-read.py <server samples> <client samples> [<seconds before> [<seconds after>]]
#
# Lines up what stall-sample.py took on the server and on the client around
# the moment a port went silent: the first sample in which the server's port
# received under a tenth of what it had been receiving.  For each fifth of a
# second, each way across the wire for the silent port and the other one:
# what the sender put on the wire and what the receiver took off it, in
# frames, then the counters that moved at all.
import sys

def load(f):
    rows = []
    for l in open(f):
        p = l.split()
        try:
            t = float(p[0])
        except (ValueError, IndexError):
            continue
        d = {}
        for kv in p[1:]:
            k, _, v = kv.partition('=')
            d[k] = int(v) if v.lstrip('-').isdigit() else v
        rows.append((t, d))
    return rows

srv, cl = load(sys.argv[1]), load(sys.argv[2])
before = float(sys.argv[3]) if len(sys.argv) > 3 else 3
after = float(sys.argv[4]) if len(sys.argv) > 4 else 6
PAIR = {'enp161s0f0': 'ens3np0', 'enp164s0f0': 'ens6np0'}

def rate(rows, key):	# (t, per-second) between samples
    o = []
    for (t0, a), (t1, b) in zip(rows, rows[1:]):
        if key in a and key in b:
            o.append((t1, (b[key] - a[key]) / (t1 - t0)))
    return o

# the onset: a port that had been taking 100 thousand frames a second takes under a tenth of it
onset = None
for sp in PAIR:
    r = rate(srv, sp + '.rx_packets'); hot = 0
    for i, (t, v) in enumerate(r):
        if v > 100000:
            hot += 1
        elif hot > 25 and v < 0.1 * max(x for _, x in r[max(0, i - 25):i]):
            other = [o for o in PAIR if o != sp][0]
            ro = dict(rate(srv, other + '.rx_packets'))
            if ro.get(t, 0) > 100000:
                if onset is None or t < onset[0]:
                    onset = (t, sp)
                break
            hot = 0	# both ports stopped: the load's own start or end
if not onset:
    print('no port went silent while the other carried on'); sys.exit(0)
t0, sp = onset; cp = PAIR[sp]; so = [o for o in PAIR if o != sp][0]; co = PAIR[so]
print('silent: %s (client %s), from %.1f; the other port is %s' % (sp, cp, t0, so))

def near(rows, t):
    return min(rows, key=lambda r: abs(r[0] - t))

def at(rows, key, t):	# per-second rate over the sample interval ending nearest t
    best = None
    for (ta, a), (tb, b) in zip(rows, rows[1:]):
        if key in a and key in b and (best is None or abs(tb - t) < abs(best[0] - t)):
            best = (tb, (b[key] - a[key]) / (tb - ta))
    return best[1] if best else float('nan')

print('\nthousand frames a second, each fifth of a second:')
print('%7s | %s: client sent, server took | server sent, client took | %s: client sent, server took' % ('t', sp, so))
t = t0 - before
while t <= t0 + after:
    print('%+7.1f | %9.0f %9.0f | %9.1f %9.1f | %9.0f %9.0f' % (
        t - t0, at(cl, cp + '.tx_packets_phy', t) / 1e3, at(srv, sp + '.rx_packets', t) / 1e3,
        at(srv, sp + '.tx_packets', t) / 1e3, at(cl, cp + '.rx_packets_phy', t) / 1e3,
        at(cl, co + '.tx_packets_phy', t) / 1e3, at(srv, so + '.rx_packets', t) / 1e3))
    t += 0.2

def moved(rows, name, lo, hi, skip):
    a, b = near(rows, t0 + lo)[1], near(rows, t0 + hi)[1]; o = []
    for k in sorted(b):
        if isinstance(b[k], int) and k in a and b[k] != a[k] and not any(s in k for s in skip):
            o.append('%s +%d' % (k, b[k] - a[k]))
    print('\n%s, %+.1f to %+.1f s: %s' % (name, lo, hi, '; '.join(o) or 'nothing'))

SKIP = ('_bytes', 'rx_packets', 'tx_packets', 'irqs', 'InSegs', 'OutSegs', 'TCPDelivered', 'TCPPureAcks',
        'DelayedACKs', 'rx_pp_', 'corrected_bits', 'fifo_tc0_packets')
for lo, hi in ((-before, -0.4), (-0.4, 1.0), (1.0, after)):
    moved(srv, 'server', lo, hi, SKIP); moved(cl, 'client', lo, hi, SKIP)
print('\nFEC bits corrected a second at the client, %s / %s: before %.0f / %.0f, across the onset %.0f / %.0f' % (
    cp, co, at(cl, cp + '.rx_corrected_bits_phy', t0 - 1), at(cl, co + '.rx_corrected_bits_phy', t0 - 1),
    at(cl, cp + '.rx_corrected_bits_phy', t0 + 0.2), at(cl, co + '.rx_corrected_bits_phy', t0 + 0.2)))
for name, rows, ps in (('server', srv, (sp, so)), ('client', cl, (cp, co))):
    for p in ps:
        seen = []
        for t, d in rows:
            v = d.get(p + '.neigh')
            if v is not None and t0 - before <= t <= t0 + after and (not seen or seen[-1][1] != v):
                seen.append((t - t0, v))
        print('%s %s neighbours: %s' % (name, p, '  '.join('%+.1f %s' % x for x in seen)))
