#!/usr/bin/python3 -I
# price.py RUN FUNC...: cycles a WRITE under each FUNC (a sample counts once
# for each FUNC on its stack, inlined frames folded), in one pass, plus each
# FUNC's cycles outside svc_tcp_recvfrom and outside schedule_timeout.
import collections, lzma, re, sys
L = '/var/tmp/zc-run-logs'; r, F = sys.argv[1], sys.argv[2:]
W = int(re.search(r'WRITEs ([0-9]+)', open(f'{L}/{r}/pmu.log').read()).group(1)) * 8.0
tot = collections.Counter(); bare = collections.Counter()
def take(p, fr):
    s = set(fr)
    for f in F:
        if f in s:
            tot[f] += p
            i = fr.index(f)
            if not {'svc_tcp_recvfrom', 'schedule_timeout', 'svc_process'} & set(fr[:i]): bare[f] += p
per = None; fr = []
for l in lzma.open(f'{L}/{r}/pmu-g.script.xz', 'rt'):
    m = re.match(r'^\s*([0-9]+)\s+cycles', l)
    if m:
        if per is not None: take(per, fr)
        per = int(m.group(1)); fr = []; continue
    if not l.strip():
        if per is not None: take(per, fr)
        per = None; fr = []; continue
    q = l.split()
    if per is not None and len(q) > 1 and q[-1] != '(inlined)': fr.append(re.sub(r'\+0x[0-9a-f]+$', '', q[1]))
if per is not None: take(per, fr)
print('%s, cycles a WRITE: under FUNC; and under FUNC but not under svc_tcp_recvfrom, schedule_timeout or svc_process' % r)
for f in F: print('  %7.0f %7.0f  %s' % (tot[f] / W, bare[f] / W, f))
