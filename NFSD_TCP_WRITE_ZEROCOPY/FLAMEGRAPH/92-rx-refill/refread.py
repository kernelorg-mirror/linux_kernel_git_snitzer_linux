#!/usr/bin/python3 -I
# refread.py RUN...: round 92's refill.bt output (refill.txt), per run.
# Rings (the refill's rxr): refills, buffers still posted when refilled
# (fewer than 32, none), refills that posted less than asked.  NAPI (by
# device): polls, frames a poll, the share at full budget, gaps over 50 us
# before a full-budget poll, polls over 50 us.  The histograms as printed,
# and the page pool's and the retry timer's counts.
import collections, re, sys
L = '/var/tmp/zc-run-logs'
for r in sys.argv[1:]:
    t = open(f'{L}/{r}/refill.txt').read()
    log = open(f'{L}/{r}/pmu.log').read()
    drops = sum(int(x) for x in re.findall(r'ring drops (\d+)', log))
    m = collections.defaultdict(dict); scal = {}
    for k, key, v in re.findall(r'^@(\w+)\[(\d+)\]: (\S+)$', t, re.M):
        m[k][int(key)] = v
    for k, v in re.findall(r'^@(\w+): (\d+)$', t, re.M):
        scal[k] = int(v)
    print(f'== {r}: {re.search(r"^total.*", log, re.M).group(0)}; twoport ring drops in the window {drops}')
    rings = sorted(m['size'])
    tot = collections.Counter()
    for x in rings:
        g = lambda k: int(m[k].get(x, 0))
        for k in ('refills', 'left_under32', 'left_zero', 'refill_short', 'short_by', 'posted'):
            tot[k] += g(k)
        print(f'  ring {x:x}: size {g("size")}, refills {g("refills")}, left <32 {g("left_under32")}, left 0 {g("left_zero")}, short {g("refill_short")} by {g("short_by")}, posted {g("posted")}')
    print(f'  rings: refills {tot["refills"]}, left <32 {tot["left_under32"]}, left 0 {tot["left_zero"]}, short {tot["refill_short"]} (by {tot["short_by"]} buffers), posted {tot["posted"]}')
    byd = collections.defaultdict(collections.Counter)
    for n, dev in m['dev'].items():
        for k in ('polls', 'frames', 'full', 'gap_full_over50', 'dur_over50'):
            byd[dev][k] += int(m[k].get(n, 0))
    for dev, c in sorted(byd.items()):
        print(f'  {dev}: polls {c["polls"]}, frames/poll {c["frames"]/max(c["polls"],1):.1f}, full budget {100*c["full"]/max(c["polls"],1):.1f}%, gaps >50 us before a full poll {c["gap_full_over50"]}, polls >50 us {c["dur_over50"]}')
    print('  ' + ', '.join(f'{k} {v}' for k, v in sorted(scal.items())))
    for h in ('left_h', 'gap_full_us', 'dur_us'):
        i = t.find(f'@{h}:')
        if i < 0: continue
        body = t[i:].split('\n\n')[0].splitlines()[1:]
        rows = [l for l in body if re.search(r'\|\s*$|\|@', l) and not re.match(r'^\S.*\s0 \|', l)]
        print(f'  @{h}: ' + '; '.join(re.sub(r'\s+', ' ', l.split('|')[0]).strip() for l in rows))
