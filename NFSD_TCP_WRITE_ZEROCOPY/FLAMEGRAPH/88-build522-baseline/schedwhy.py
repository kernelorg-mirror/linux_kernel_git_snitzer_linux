#!/usr/bin/python3 -I
# schedwhy.py RUN [N]: cycles a WRITE under __schedule() by the first
# frame above it that is not scheduler or wait plumbing: why the CPU
# switched.  From RUN's pmu-g.script.xz.
import collections, lzma, re, sys
L='/var/tmp/zc-run-logs'; r = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 20
W = int(re.search(r'WRITEs ([0-9]+)', open(f'{L}/{r}/pmu.log').read()).group(1)) * 8.0
P = re.compile(r'^(__schedule|schedule|schedule_timeout|schedule_preempt|preempt_schedule|io_schedule|__wait|wait_for|do_wait|__cond_resched|_cond_resched|__flush_work|flush_work|__flush_workqueue|rwsem_down|__rwsem|down_|__down|mutex|__mutex|schedule_hrtimeout|do_nanosleep|hrtimer_nanosleep|el1h|el1_|irqentry|arm64_preempt|.*_slowpath)')
why = collections.Counter(); tot = 0
def take(p, fr):
    global tot
    if '__schedule' not in fr: return
    i = fr.index('__schedule'); tot += p
    k = '?'
    for f in fr[i + 1:]:
        if not P.match(f): k = f; break
    why[k] += p
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
print('%s under __schedule: %.0f cycles a WRITE' % (r, tot / W))
for k, v in why.most_common(N): print('   %6.0f %5.1f%%  %s' % (v / W, 100 * v / tot, k))
