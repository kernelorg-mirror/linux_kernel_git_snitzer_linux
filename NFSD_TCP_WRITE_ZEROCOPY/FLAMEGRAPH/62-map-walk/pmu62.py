#!/usr/bin/python3 -I
# pmu62.py A.script WRITES_A [B.script WRITES_B] [TOP]: nfsd's CPUs' samples
# (perf script -F ip,sym,period,event of cycles, instructions, L1 and L2
# refills, as round 55's run170) by symbol, self, in cycles and instructions a
# WRITE (WRITES the WRITEs in the 8 s sampled), with IPC and L2 refills a
# WRITE; the idle loop left out.  With two scripts, side by side and B - A,
# for nfsd's TOP (30) symbols of either.  "the map walk" sums the walk's
# functions whether inlined or not (round 55: blk_map_iter_next out of line;
# round 62: inlined into blk_rq_dma_map_iter_next and _start), so it compares.
import collections, sys
IDLE = set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
WALK = set('''blk_map_iter_next __blk_map_iter_next blk_rq_dma_map_iter_next
blk_rq_dma_map_iter_start nvme_pci_setup_data_sgl dma_map_phys dma_direct_map_phys
bio_split_io_at'''.split())
def load(path, writes):
    c = collections.defaultdict(collections.Counter)
    for l in open(path):
        f = l.split()
        if len(f) < 4:
            continue
        try:
            p = int(f[0])
        except ValueError:
            continue
        ev, sym = f[1].rstrip(':').split(':')[0], f[3]
        if sym in IDLE:
            continue
        c[ev][sym] += p
        if sym in WALK:
            c[ev]['(the map walk)'] += p
        c[ev]['(all of nfsd)'] += p
    return {e: {s: v / writes for s, v in d.items()} for e, d in c.items()}
args = sys.argv[1:]
runs = [(args[0], load(args[0], float(args[1])))]
if len(args) >= 4:
    runs.append((args[2], load(args[2], float(args[3]))))
top = int(args[4]) if len(args) >= 5 else (int(args[2]) if len(args) == 3 else 30)
cy, ins, l2 = 'cycles', 'instructions', 'l2d_cache_refill'
syms = ['(all of nfsd)', '(the map walk)']
for _, d in runs:
    for s, _v in sorted(d[cy].items(), key=lambda kv: -kv[1]):
        if s not in syms and len([x for x in syms if not x.startswith('(')]) < top * len(runs):
            if s not in syms:
                syms.append(s)
            if len([x for x in syms if not x.startswith('(')]) >= top:
                break
def row(d, s):
    c, i = d[cy].get(s, 0.0), d[ins].get(s, 0.0)
    return c, i, (i / c if c else 0.0), d.get(l2, {}).get(s, 0.0)
hdr = 'cycles a WRITE, instructions a WRITE, IPC, L2 refills a WRITE'
print('runs: ' + ' | '.join(p for p, _ in runs))
print('%-34s ' % 'symbol (self)' + ' | '.join('%9s %9s %5s %7s' % ('cyc', 'ins', 'IPC', 'L2') for _ in runs) + ('   %9s %9s' % ('d cyc', 'd ins') if len(runs) == 2 else ''))
for s in syms:
    r = [row(d, s) for _, d in runs]
    line = '%-34s ' % s[:34] + ' | '.join('%9.0f %9.0f %5.2f %7.1f' % x for x in r)
    if len(r) == 2:
        line += '   %+9.0f %+9.0f' % (r[1][0] - r[0][0], r[1][1] - r[0][1])
    print(line)
