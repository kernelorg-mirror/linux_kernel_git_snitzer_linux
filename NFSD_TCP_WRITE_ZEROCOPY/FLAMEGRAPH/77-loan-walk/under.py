#!/usr/bin/python3 -I
# under.py RUN FUNC [N]: cycles a WRITE under FUNC by the leaf they end in,
# and by FUNC's direct callee on the way (inlined frames folded).
import collections, lzma, re, sys
L='/var/tmp/zc-run-logs'; r, F = sys.argv[1], sys.argv[2]; N = int(sys.argv[3]) if len(sys.argv) > 3 else 15
W = int(re.search(r'WRITEs ([0-9]+)', open(f'{L}/{r}/pmu.log').read()).group(1)) * 8.0
leaf = collections.Counter(); child = collections.Counter(); callers = collections.Counter(); tot = 0
def take(p, fr):
    global tot
    if F not in fr: return
    i = fr.index(F); tot += p
    leaf[fr[0]] += p; child[fr[i - 1] if i else '(self)'] += p
    callers[fr[i + 1] if i + 1 < len(fr) else '?'] += p
per = None; fr = []
for l in lzma.open(f'{L}/{r}/pmu-g.script.xz', 'rt'):
    m = re.match(r'^\s*([0-9]+)\s+cycles', l)
    if m:
        if per is not None: take(per, fr)
        per = int(m.group(1)); fr = []; continue
    if not l.strip():
        if per is not None: take(per, fr)
        per = None; fr = []; continue
    p = l.split()
    if per is not None and len(p) > 1 and p[-1] != '(inlined)': fr.append(re.sub(r'\+0x[0-9a-f]+$', '', p[1]))
if per is not None: take(per, fr)
print('%s under %s: %.0f cycles a WRITE' % (r, F, tot / W))
for name, c in (('called from', callers), ('direct callee', child), ('leaf', leaf)):
    print(' by %s:' % name)
    for k, v in c.most_common(N): print('   %6.0f %5.1f%%  %s' % (v / W, 100 * v / tot, k))
