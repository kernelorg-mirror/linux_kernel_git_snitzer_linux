#!/usr/bin/python3 -I
# msread.py RUN...: round 91's 10 ms samples (run423.sh).  Per port, from
# srv-ring-ms.tsv: the share of 10 ms intervals with tail drops, how the
# drops gather (the worst interval, the share in the worst 1% of
# intervals, the longest run of intervals in a row with drops), the
# frames per interval and their spread, and whether drops come with more
# frames (corr, same interval).  An interval with no frames at all on a
# busy port is counted: many would mean the card's counters move in steps
# coarser than the sampling.  From napi.txt: the ring CPUs' polls per
# 10 ms, the share that used the whole budget, and corr with the drops of
# both ports (buckets matched by index: both start together).  Then each
# arm's mean.
import collections, re, statistics as st, sys
L = '/var/tmp/zc-run-logs'
def corr(x, y):
    n = min(len(x), len(y)); x, y = x[:n], y[:n]
    if n < 3 or st.pstdev(x) == 0 or st.pstdev(y) == 0: return float('nan')
    mx, my = st.mean(x), st.mean(y)
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / n / st.pstdev(x) / st.pstdev(y)
arm = collections.defaultdict(lambda: collections.defaultdict(list))
for r in sys.argv[1:]:
    d = f'{L}/{r}'
    v = re.search(r'wq-(\w+)', open(f'{d}/results.tsv').read()).group(1)
    log = open(f'{d}/pmu.log').read()
    rows = [l.rstrip('\n').split('\t') for l in open(f'{d}/srv-ring-ms.tsv')]
    h = rows[0]; D = [[float(x or 0) for x in row] for row in rows[1:]]
    dt = [(q[0] - p[0]) * 1000 for p, q in zip(D, D[1:])]
    print(f'== {r} {v}: {re.search(r"^total.*", log, re.M).group(0)}; {len(dt)} intervals, {st.mean(dt):.1f} ms mean, {max(dt):.1f} ms longest')
    both = None
    for p, dev in (('a', 'enp161s0f0'), ('b', 'enp164s0f0')):
        def col(rx):
            ix = [i for i, c in enumerate(h) if c.startswith(dev + ':') and re.search(rx, c)]
            return [sum(q[i] - pp[i] for i in ix) for pp, q in zip(D, D[1:])]
        drop = col(r'tail_drop'); fr = col(r'ring_\d+_rx_packets$'); ppd = col(r'pp_drop')
        n = len(drop); T = sum(drop) or 1
        top = sorted(drop)[-max(1, n // 100):]
        run = best = 0
        for x in drop:
            run = run + 1 if x > 0 else 0; best = max(best, run)
        zero = sum(1 for x in fr if x == 0)
        cvf = st.pstdev(fr) / st.mean(fr) if st.mean(fr) else float('nan')
        c = corr(drop, fr)
        both = drop if both is None else [a + b for a, b in zip(both, drop)]
        print(f' port {p}: drops {sum(drop):.0f} in {100*sum(1 for x in drop if x > 0)/n:.0f}% of intervals, worst {max(drop):.0f}, worst 1% hold {100*sum(top)/T:.0f}%,'
              f' longest run {best} ({best*10} ms); frames/10 ms {st.mean(fr):.0f} (cv {100*cvf:.0f}%, {zero} empty); pp drops {sum(ppd):.0f}; corr(drops, frames) {c:+.2f}')
        a = arm[v]; a['share'].append(100 * sum(1 for x in drop if x > 0) / n); a['top'].append(100 * sum(top) / T)
        a['run'].append(best); a['cvf'].append(100 * cvf); a['corr'].append(c); a['drops'].append(sum(drop))
    polls = collections.Counter(); full = collections.Counter(); work = collections.Counter()
    for l in open(f'{d}/napi.txt'):
        m = re.match(r'@(polls|full|work)\[(\d+), (\d+)\]: (\d+)', l)
        if m:
            {'polls': polls, 'full': full, 'work': work}[m.group(1)][int(m.group(3))] += int(m.group(4))
    if polls:
        B = sorted(polls)[1:-1]
        fb = [full[b] for b in B]; pb = [polls[b] for b in B]
        fs = sum(fb) / max(sum(pb), 1)
        print(f' NAPI (ring CPUs): {st.mean(pb):.0f} polls/10 ms, {100*fs:.1f}% used the whole budget, buckets with any {100*sum(1 for x in fb if x)/len(fb):.0f}%;'
              f' corr(drops, full polls) {corr(both, fb):+.2f}, with the bucket after {corr(both, fb[1:]):+.2f}')
        arm[v]['full'].append(100 * fs); arm[v]['ncorr'].append(corr(both, fb))
print('\n== by arm (mean over runs and ports)')
for v, a in arm.items():
    m = lambda k: st.mean([x for x in a[k] if x == x] or [float('nan')])
    print(f' {v:3s} drops/port {m("drops"):6.0f}, intervals with drops {m("share"):4.0f}%, worst 1% hold {m("top"):3.0f}%, longest run {m("run"):4.1f},'
          f' frames cv {m("cvf"):3.0f}%, corr(drops, frames) {m("corr"):+.2f}; full-budget polls {m("full"):4.1f}%, corr {m("ncorr"):+.2f}')
