#!/usr/bin/env python3
# pagelife-read.py <pagelife.bt output>...: each stage's cycles, mean time and
# median, and by Little's law the region's pages it holds on average (its
# summed time over the time traced, times 8 for the sampling).
import re, sys
for f in sys.argv[1:]:
    t = n = None; S = {}; N = {}; H = {}; cur = None
    mult = {'': 1, 'K': 1024, 'M': 1024 ** 2, 'G': 1024 ** 3}
    for l in open(f):
        if l.startswith('traced_us'): t = int(l.split()[1].rstrip(','))
        m = re.match(r'^@sum\[(.*)\]: (-?\d+)', l)
        if m: S[m.group(1)] = int(m.group(2))
        m = re.match(r'^@n\[(.*)\]: (\d+)', l)
        if m: N[m.group(1)] = int(m.group(2))
        m = re.match(r'^@us\[(.*)\]:', l)
        if m: cur = m.group(1); H[cur] = []; continue
        m = re.match(r'^\[(\d+)([KMG]?)[,\]]', l)
        c = re.search(r'[\])]\s+(\d+)\s+\|', l)
        if m and cur and c: H[cur].append((int(m.group(1)) * mult[m.group(2)], int(c.group(1))))
    print(f, 'traced', t / 1e6 if t else '?', 's;', ', '.join('%s %d' % kv for kv in sorted(N.items())))
    tot = 0
    for k in sorted(S):
        c = sum(c for _, c in H.get(k, [])) or None
        if c is None:
            c = N.get({'2-4 borrow to put, no WRITE seen': 'loaned, no WRITE seen', '2-5 borrow to repost, no put seen': 'loaned, no put seen'}.get(k, ''), 0)
        med = None; cum = 0
        for lo, x in H.get(k, []):
            cum += x
            if med is None and cum >= c / 2: med = lo
        pages = S[k] / t * 8 if t else 0
        tot += pages
        print('  %-36s cycles %8d  mean %7.0f us  median [%s, %s) us  pages held %7.0f' % (k, c, S[k] / c if c else 0, med, 2 * med if med else '-', pages))
    print('  %-36s %s pages held in all (sampled 1 in 8)' % ('', round(tot)))
