#!/usr/bin/python3
# hol-hist.py <hol.out> [cap_us]: each class's empty time from its @len
# histogram (a bucket at its midpoint), gaps of cap_us (100000) or more
# left out: hol.bt before the fix of two issues at once read a gap from
# the trace's start (t2-r566, r567).
import re, sys
NAMES = ['A in nfsd', 'B read, not writing', 'C being read', 'D1 behind another export',
         'D2 behind its own export', 'E connection idle', 'F no WRITE seen']
text = open(sys.argv[1]).read(); cap = int(sys.argv[2]) if len(sys.argv) > 2 else 100000
span = int(re.search(r'traced_us (\d+)', text).group(1))
u = {'': 1, 'K': 1024, 'M': 1 << 20}
res = {}
for m in re.finditer(r'^@len\[(\d)\]:\n((?:\[.*\n)+)', text, re.M):
    g = us = drop = 0
    for lo, lu, hi, hu, n in re.findall(r'^\[(\d+)([KM]?)(?:, (\d+)([KM]?))?[)\]]\s+(\d+)', m.group(2), re.M):
        a = int(lo) * u[lu]; b = int(hi) * u[hu] if hi else a + 1
        if a >= cap:
            drop += int(n); continue
        g += int(n); us += int(n) * (a + b) / 2
    res[int(m.group(1))] = (g, us, drop)
tot = sum(v[1] for v in res.values())
print(f'span {span / 1e6:.2f} s; drives empty {100 * tot / (8 * span):.1f}% of the span (gaps under {cap // 1000} ms)')
for k in sorted(res):
    g, us, drop = res[k]
    print(f'  {NAMES[k]:28s} {g:6d} gaps {100 * us / tot:5.1f}% of empty' + (f' ({drop} over the cap left out)' if drop else ''))
