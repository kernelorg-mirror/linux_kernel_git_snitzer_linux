#!/usr/bin/python3 -I
# ringsec.py RUN...: round 91's per-second samples (run411.sh), over the
# 40 s after the 10 s warm-up, less any second in which the client sends
# under 90% of its median (the load's end, when it falls in the 40 s).  Per port: the server's tail drops (in how
# many seconds, the worst second, the share of the worst 3), page pool and
# FIFO drops, and its rings' spread; the client's sends on that port (the
# per-second spread of its packets, the queues that carry them and the
# share of the busiest, xmit_more and stopped), and the correlation of
# drops with the client's packets second by second.  Then each arm's mean.
import collections, math, re, statistics as st, sys
L = '/var/tmp/zc-run-logs'
PORTS = (('a', 'enp161s0f0', 'ens3np0', range(0, 24)), ('b', 'enp164s0f0', 'ens6np0', range(24, 48)))
def load(f):
    rows = [l.rstrip('\n').split('\t') for l in open(f)]
    h = rows[0]; d = [[float(x) if x else 0.0 for x in r] for r in rows[1:]]
    return h, d
def rates(h, d, keep):
    """per-interval differences of the columns keep() selects: [(t, {col: per-s})]"""
    ix = [i for i, c in enumerate(h) if i and keep(c)]
    out = []
    for p, q in zip(d, d[1:]):
        dt = q[0] - p[0]
        out.append((q[0], {h[i]: (q[i] - p[i]) / dt for i in ix}))
    return out
def window(r):
    return r[10:50]
def corr(x, y):
    if len(x) < 3 or st.pstdev(x) == 0 or st.pstdev(y) == 0: return float('nan')
    mx, my = st.mean(x), st.mean(y)
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / len(x) / st.pstdev(x) / st.pstdev(y)
arm = collections.defaultdict(lambda: collections.defaultdict(list))
for r in sys.argv[1:]:
    d = f'{L}/{r}'
    log = open(f'{d}/pmu.log').read()
    v = re.search(r'wq-(\w+)', open(f'{d}/results.tsv').read()).group(1)
    sh, sd = load(f'{d}/srv-ring-sec.tsv'); ch, cd = load(f'{d}/cli-txq-sec.tsv')
    print(f'== {r} {v}: {re.search(r"^total.*", log, re.M).group(0)}')
    for p, sdev, cdev, near in PORTS:
        sr = window(rates(sh, sd, lambda c: c.startswith(sdev + ':')))
        cr = rates(ch, cd, lambda c: c.startswith(cdev + ':'))
        tail = [sum(v for k, v in x.items() if 'tail_drop' in k) for _, x in sr]
        pp = sum(sum(v for k, v in x.items() if 'pp_drop' in k) for _, x in sr)
        fifo = sum(sum(v for k, v in x.items() if 'fifo' in k) for _, x in sr)
        rings = collections.Counter()
        for _, x in sr:
            for k, val in x.items():
                m = re.search(r'ring_(\d+)_rx_hw_tail_drop', k)
                if m: rings[m.group(1)] += val
        # the client's seconds nearest the server's
        cw = [min(cr, key=lambda c: abs(c[0] - t))[1] for t, _ in sr] if cr else []
        pk = [sum(v for k, v in x.items() if k.endswith('_packets') and re.search(r':tx\d+_packets', k)) for x in cw]
        md = st.median(pk); ok = [i for i, x in enumerate(pk) if x >= 0.9 * md]
        pk = [pk[i] for i in ok]; cw = [cw[i] for i in ok]; tail = [tail[i] for i in ok]
        T = sum(tail); top3 = sum(sorted(tail)[-3:])
        qtot = collections.Counter(); xm = 0; stp = 0
        for x in cw:
            for k, val in x.items():
                m = re.search(r':tx(\d+)_(packets|xmit_more|stopped)$', k)
                if not m: continue
                if m.group(2) == 'packets': qtot[int(m.group(1))] += val
                elif m.group(2) == 'xmit_more': xm += val
                else: stp += val
        P = sum(qtot.values()) or 1
        busy = [q for q, n in qtot.items() if n > 0.01 * P]
        qc = {}
        for l in open(f'{d}/pmu/cli-xps-{p}.txt'):
            f = l.split()
            if len(f) == 2 and f[0].isdigit():
                m = int(f[1].replace(',', ''), 16)
                if m and m & (m - 1) == 0: qc[int(f[0])] = m.bit_length() - 1
        far = sum(n for q, n in qtot.items() if q in qc and qc[q] not in near) / P
        cv = st.pstdev(pk) / st.mean(pk) if pk and st.mean(pk) else float('nan')
        c = corr(tail, pk)
        print(f' port {p}: tail drops {T:.0f} in {sum(1 for t in tail if t > 0)} of {len(tail)} s, worst second {max(tail):.0f}, worst 3 s {100*top3/max(T,1):.0f}%;'
              f' rings {dict(sorted((k, int(v)) for k, v in rings.items()))}; pp drops {pp:.0f}, fifo {fifo:.0f}')
        print(f'         client: packets/s {st.mean(pk)/1e6:.2f}M (cv {100*cv:.1f}%), queues over 1% {len(busy)},'
              f' busiest {100*max(qtot.values())/P:.0f}%, far queues {100*far:.1f}%, xmit_more {100*xm/P:.1f}%, stopped {stp:.0f};'
              f' corr(drops, packets) {c:+.2f}')
        a = arm[v]
        a['drops'].append(T); a['secs'].append(sum(1 for t in tail if t > 0)); a['worst'].append(max(tail))
        a['cv'].append(100 * cv); a['busy'].append(len(busy)); a['top'].append(100 * max(qtot.values()) / P)
        a['xm'].append(100 * xm / P); a['corr'].append(c)
print('\n== by arm (mean over runs and ports)')
for v, a in arm.items():
    print(f' {v:3s} drops/port {st.mean(a["drops"]):6.0f}, seconds with drops {st.mean(a["secs"]):4.1f}, worst second {st.mean(a["worst"]):5.0f},'
          f' client cv {st.mean(a["cv"]):4.1f}%, queues over 1% {st.mean(a["busy"]):4.1f}, busiest {st.mean(a["top"]):4.1f}%,'
          f' xmit_more {st.mean(a["xm"]):4.1f}%, corr {st.mean([x for x in a["corr"] if x == x] or [float("nan")]):+.2f}')
