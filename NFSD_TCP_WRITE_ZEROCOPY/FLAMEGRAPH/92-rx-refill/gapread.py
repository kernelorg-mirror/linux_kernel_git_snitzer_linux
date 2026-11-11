#!/usr/bin/python3 -I
# gapread.py RUN...: napigap.txt of each run, per port: polls, the share
# of them full, the gap to a poll by what the poll before it did (median
# and the share at 32 us or more, from the log2 histograms, so to a power
# of two), runs of full polls, frames a us inside them, the longest gap
# inside one; and NET_RX's squeezes on the ring CPUs from softnet_stat.
import re, sys
L = '/var/tmp/zc-run-logs'
RX = [0, 2, 4, 6, 56, 58, 60, 62]
KIND = {0: 'after empty', 1: 'after part', 2: 'after full'}

def parse(path):
    h, s, cur = {}, {}, None
    for l in open(path):
        m = re.match(r'^@(\w+)\[([^\]]*)\]: (-?\d+)$', l)
        if m:
            s[(m.group(1), m.group(2))] = int(m.group(3)); cur = None; continue
        m = re.match(r'^@(\w+)\[([^\]]*)\]:$', l)
        if m:
            cur = (m.group(1), m.group(2)); h[cur] = []; continue
        m = re.match(r'^[\[(](-?\d+)K?, (-?\d+|\.\.\.)K?[\])]?\s+(\d+)', l)
        if m and cur:
            lo = l.split(',')[0].strip('[( ')
            mult = 1024 if lo.endswith('K') else 1
            h[cur].append((int(lo.rstrip('K')) * mult, int(m.group(3))))
    return h, s

def pct(b, q):
    t = sum(c for _, c in b); a = 0
    for lo, c in b:
        a += c
        if a >= q * t: return lo
    return None

def share_ge(b, x):
    t = sum(c for _, c in b)
    return 100.0 * sum(c for lo, c in b if lo >= x) / t if t else 0

def softnet(path):
    r = {}
    for i, l in enumerate(open(path)):
        f = [int(x, 16) for x in l.split()]
        cpu = f[12] if len(f) > 12 else i
        r[cpu] = f
    return r

for run in sys.argv[1:]:
    d = f'{L}/{run}'
    h, s = parse(f'{d}/napigap.txt')
    print(f'{run}:')
    for p, name in ((0, 'a'), (1, 'b')):
        w = h.get(('work', str(p)), [])
        np = sum(c for _, c in w)
        full = sum(c for lo, c in w if lo >= 64)
        print(f'  port {name}: {np} polls, {100.0*full/np if np else 0:.1f}% full,'
              f' {sum(lo*c for lo,c in w)/np if np else 0:.0f}+ frames a poll')
        for k in (2, 1, 0):
            b = h.get(('gap', f'{p}, {k}'), [])
            n = sum(c for _, c in b)
            if not n: continue
            print(f'    gap {KIND[k]:11s}: {n:8d}, median {pct(b,.5)} us+, p99 {pct(b,.99)} us+,'
                  f' >=32 us {share_ge(b,32):.2f}%, >=64 us {share_ge(b,64):.3f}%')
        us, fr = s.get(('inrun_us', str(p))), s.get(('inrun_frames', str(p)))
        rl = h.get(('runlen', str(p)), [])
        if us:
            print(f'    in runs of full polls: {fr/us:.2f} frames a us ({us/fr*64:.1f} us a full poll),'
                  f' longest gap {s.get(("inrun_maxgap", str(p)))} us; runs {sum(c for _,c in rl)},'
                  f' median length {pct(rl,.5)}+, p99 {pct(rl,.99)}+')
        aw = h.get(('after_wait', str(p)), [])
        if aw:
            print(f'    after a wait >= 64 us: {sum(c for _,c in aw)} polls, {share_ge(aw,64):.1f}% full')
    try:
        a, b = softnet(f'{d}/softnet.0'), softnet(f'{d}/softnet.1')
        print('  softnet on the ring CPUs (processed, dropped, squeezed):',
              ' '.join(f'{c}:{b[c][0]-a[c][0]}/{b[c][1]-a[c][1]}/{b[c][2]-a[c][2]}' for c in RX if c in a and c in b))
    except FileNotFoundError:
        pass
