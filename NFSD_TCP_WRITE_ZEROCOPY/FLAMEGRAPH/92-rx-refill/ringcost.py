#!/usr/bin/python3 -I
# ringcost.py RUN...: cycles and instructions a WRITE on the receive rings'
# CPUs (0, 2, 4, 6, 56, 58, 60, 62) and the transmit rings' (the odd ones
# of 0-7 and 56-63), idle out, from run465.sh's pmu.script.xz (8 s of
# perf at the window's rate; WRITEs as twoport prints them, a second), and
# on the receive CPUs by area: the refill (xeu_rx_refill, its doorbell
# inlined), the page pool's allocation, the poll itself, and the rest.
import collections, lzma, re, sys
L = '/var/tmp/zc-run-logs'
RX = {0, 2, 4, 6, 56, 58, 60, 62}
TX = {1, 3, 5, 7, 57, 59, 61, 63}
IDLE = set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
G = [('refill', r'^xeu_rx_refill'),
     ('page_pool alloc', r'^(page_pool_alloc|__page_pool_alloc|page_pool_refill|page_pool_fragment|rx_region_(get|alloc|take))'),
     ('poll', r'^(xeu_rx_poll|xeu_rx_gro_frags|xeu_rx_prefetch)'),
     ('gro/tcp/ip', r'^(napi_gro|dev_gro|skb_gro|tcp|__tcp|ip_|__ip|inet_gro|gro_)')]
for run in sys.argv[1:]:
    W = int(re.search(r'WRITEs ([0-9]+)', open(f'{L}/{run}/pmu.log').read()).group(1)) * 8.0
    mid = open(f'{L}/{run}/rx_refill_mid.txt').read().strip()
    tot = collections.Counter(); area = collections.Counter()
    for l in lzma.open(f'{L}/{run}/pmu.script.xz', 'rt'):
        f = l.split()
        if len(f) < 5: continue
        m = re.match(r'\[(\d+)\]', f[0])
        if not m: continue
        cpu = int(m.group(1)); per = int(f[1]); ev = f[2].rstrip(':'); sym = f[4]
        if sym in IDLE: continue
        side = 'rx' if cpu in RX else 'tx' if cpu in TX else None
        if not side: continue
        tot[side, ev] += per
        if side == 'rx' and ev.startswith('cycles'):
            k = 'rest'
            for n, p in G:
                if re.match(p, sym): k = n; break
            area[k] += per
    ev = sorted({e for _, e in tot})
    cyc = next(e for e in ev if e.startswith('cycles')); ins = next(e for e in ev if e.startswith('instr'))
    print(f'{run} mid {mid}: rx {tot["rx", cyc]/W:7.0f} cycles {tot["rx", ins]/W:7.0f} instructions a WRITE,'
          f' tx {tot["tx", cyc]/W:6.0f} cycles; rx by area: ' +
          ', '.join(f'{k} {area[k]/W:.0f}' for k in ('refill', 'page_pool alloc', 'poll', 'gro/tcp/ip', 'rest')))
