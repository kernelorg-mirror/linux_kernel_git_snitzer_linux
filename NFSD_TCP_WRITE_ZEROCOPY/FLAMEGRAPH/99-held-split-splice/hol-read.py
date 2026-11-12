#!/usr/bin/python3
# hol-read.py <hol.bt output>: each class's share of the drives' empty
# time and of their gaps, the gaps' median length, the medians of the
# "ahead" histograms, and each drive's empty share of the span.
import re, sys

NAMES = ['A in nfsd, before its I/O', 'B read, not yet writing',
         'C being read off its connection', 'D1 behind another export\'s record',
         'D2 behind its own export\'s record', 'E not begun, its connection idle',
         'F issued with no WRITE seen']
text = open(sys.argv[1]).read()
span = int(re.search(r'traced_us (\d+)', text).group(1))
scal = lambda m: {k: int(v) for k, v in re.findall(rf'^@{m}\[([^\]]+)\]: (\d+)$', text, re.M)}
gaps, gus, empty = scal('gaps'), scal('gap_us'), scal('empty')

def hists(name):
    out = {}
    for m in re.finditer(rf'^@{name}\[([^\]]+)\]:\n((?:\[.*\n)+)', text, re.M):
        b = []
        for lo, n in re.findall(r'^\[(\d+)[KM]?(?:, [^)\]]*)?[)\]]\s+(\d+)', m.group(2), re.M):
            b.append((int(lo), int(n)))
        out[m.group(1)] = b
    return out

def median(b):
    tot = sum(n for _, n in b); acc = 0
    for lo, n in b:
        acc += n
        if acc * 2 >= tot:
            return lo
    return None

tg, tu = sum(gaps.values()) or 1, sum(gus.values()) or 1
lens = hists('len')
print(f'span {span / 1e6:.2f} s; {tg} gaps, {tu / 1e3:.0f} ms empty in all (8 drives)')
print(f'{"class":40s} {"gaps":>7s} {"of gaps":>8s} {"of empty":>9s} {"median gap us":>14s}')
for i, nm in enumerate(NAMES):
    k = str(i)
    if k in gaps:
        md = median(lens.get(k, []))
        print(f'{nm:40s} {gaps[k]:7d} {100 * gaps[k] / tg:7.1f}% {100 * gus.get(k, 0) / tu:8.1f}% {">=" + str(md) if md is not None else "-":>14s}')
for k, b in hists('ahead').items():
    print(f'  median {k}: >= {median(b)} us ({sum(n for _, n in b)} gaps)')
print('empty share of the span, a drive: ' + ', '.join(
    f'nvme{k} {100 * v / span:.1f}%' for k, v in sorted(empty.items(), key=lambda x: int(x[0]))))
