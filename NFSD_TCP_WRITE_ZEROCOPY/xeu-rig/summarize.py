#!/usr/bin/python3
"""summarize.py <outdir>... - the sizing table from sweep.sh's summary.tsv.

One line per (rings, held buffer, budget in WRITEs, storage): the loan's
rate at one and sixteen connections beside its copy's at the same rings and
buffer, the most region pages in use, and that as a share of the L3.
Several runs of one setting are shown as their median with the range.
"""
import sys, os, collections, statistics

L3_PAGES = 32 * 1024 * 1024 // os.sysconf('SC_PAGESIZE')
rows = []
for d in sys.argv[1:]:
    f = open(os.path.join(d, 'summary.tsv'))
    hdr = f.readline().split()
    rows += [dict(zip(hdr, l.split())) for l in f if l.strip()]
idle = {}
try:
    for l in open(os.path.join(sys.argv[1], 'idle.txt')):
        x = l.split(); idle[x[0]] = [float(v) for v in x[1:]]
except OSError:
    pass

def med(v):
    v = [float(x) for x in v]
    if not v: return '-'
    m = statistics.median(v)
    return '%.0f' % m if len(v) == 1 else '%.0f (%.0f-%.0f, n=%d)' % (m, min(v), max(v), len(v))

by = collections.defaultdict(list)
for r in rows:
    by[(int(r['entries']), r['rcvbuf'], int(r['writes']), r['storage'], r['cell'])].append(r)

def cpu(rs):
    # server kernel CPU-seconds per GB over the 90 s window, less idle
    out = []
    for r in rs:
        gb = float(r['MBps']) * 90 / 1000
        if gb <= 0: continue
        i = idle.get('server30', [0, 0, 0, 0])
        out.append((float(r['srv_sys']) + float(r['srv_softirq']) - (i[1] + i[2]) * 3) / gb)
    return '%.2f' % statistics.median(out) if out else '-'

print('L3 = %d pages of %d KiB; half = %d' % (L3_PAGES, os.sysconf('SC_PAGESIZE') // 1024, L3_PAGES // 2))
fmt = '%-6s %-7s %-6s %-4s | %-22s %-22s | %-22s %-22s | %-6s %-5s | %-9s %-9s | %s'
print(fmt % ('rings', 'buffer', 'writes', 'stor', 'copy 1 conn MB/s', 'loan 1 conn MB/s', 'copy 16 conn MB/s', 'loan 16 conn MB/s',
             'used', '%L3', 'cpu copy', 'cpu loan', 'notes'))
keys = sorted({k[:4] for k in by if k[2] > 0})
for e, rb, w, st in keys:
    g = lambda cell, ww: by.get((e, rb, ww, st, cell), [])
    l1, l16, c1, c16 = g('step-on', w), g('step-on-n16', w), g('step-copy', 0), g('step-copy-n16', 0)
    loans = l1 + l16
    used = max([int(r['used_pages']) for r in loans] or [0])
    notes = []
    if any(float(r['MBps']) < 50 for r in l16): notes.append('STALL')
    if any(int(r['run_alloc_fail']) for r in loans): notes.append('region ran dry')
    td = sum(int(r['win_tail_drops']) for r in loans if r['win_tail_drops'] != '-')
    if td: notes.append('tail drops %d' % td)
    rto = sum(int(r['rto']) for r in loans)
    if rto: notes.append('rto %d' % rto)
    p = sum(int(r['paused']) for r in loans); o = sum(int(r['overrun']) for r in loans)
    if p or o: notes.append('paused %d overrun %d' % (p, o))
    print(fmt % ('4x%d' % e, rb if rb == '-' else '%dK' % (int(rb) // 1024), w, st,
                 med(r['MBps'] for r in c1), med(r['MBps'] for r in l1),
                 med(r['MBps'] for r in c16), med(r['MBps'] for r in l16),
                 used, '%d%%' % (100 * used // L3_PAGES), cpu(c16), cpu(l16), '; '.join(notes)))
