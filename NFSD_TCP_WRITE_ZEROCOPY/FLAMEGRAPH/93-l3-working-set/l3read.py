#!/usr/bin/python3 -I
# l3read.py RUN...: for each of run479.sh's runs, over twoport's 10 s
# window: the receive region's pages held (regionsample.py's 20 ms
# samples inside the window: median, 90th and 99th percentile, most) in
# pages and as a share of the 32 MiB L3; the time a held page stays held
# at the median (Little: the median held over the payload's rate); and
# the payload's way through memory: the mesh's requests to the memory
# controller (64 bytes each) and its cache fills, a byte and a line of
# payload.  Payload is the wire's bytes less each frame's headers (7740
# of 7806 at the rig's MTU of 7792), both ports summed.  A held page is
# on no shard, CPU cache or device slot, so the rings' posted buffers
# count as held.
import re, statistics as st, sys
L = '/var/tmp/zc-run-logs'
PAGE, L3 = 16384, 32 << 20
for run in sys.argv[1:]:
    d = f'{L}/{run}'
    t0 = float(open(f'{d}/mesh.t0').read())
    ev = {}
    for l in open(f'{d}/mesh.txt'):
        p = l.strip().split(',')
        if len(p) > 3 and p[0].replace('.', '').isdigit():
            ev[p[2].split('/')[1]] = float(p[0])
    wire = sum(float(l.split()[3]) for l in open(f'{d}/pmu/window.txt') if l.split()[:1] in (['a'], ['b']))
    pay = wire * 1e9 * 10 * 7740 / 7806
    h = sorted(int(l.split()[1]) for l in list(open(f'{d}/pmu/region-sec.tsv'))[1:]
               if t0 <= float(l.split()[0]) <= t0 + 10)
    q = lambda f: h[min(len(h) - 1, int(len(h) * f))]
    med = q(.5)
    peak = re.search(r'region pages in use at most[^:]*: ([0-9]+)', open(f'{d}/pmu.log').read())
    mid = open(f'{d}/rx_refill_mid.txt').read().strip() if __import__('os').path.exists(f'{d}/rx_refill_mid.txt') else '?'
    print(f'{run} mid {mid} | wire {wire:.2f} GB/s | held in the window ({len(h)} samples):'
          f' median {med} ({med*PAGE/2**20:.0f} MiB, {med*PAGE/L3:.2f} L3), p90 {q(.9)}, p99 {q(.99)}, most {h[-1]}'
          f' | kernel peak {peak.group(1) if peak else "?"} | a page held {med*PAGE/(pay/10)*1e3:.2f} ms at the median'
          f' | to memory {ev["hnf_mc_reqs"]*64/10/1e9:.1f} GB/s, {ev["hnf_mc_reqs"]*64/pay:.2f} bytes a byte'
          f' | fills {ev["hnf_cache_fill"]*64/pay:.2f} a payload line')
