#!/usr/bin/python3 -I
# irqs.py FOLDED USPERWRITE: what a profile taken with pseudo-NMI shows of the
# hard interrupts: each stack with gic_handle_irq on it, by kind of interrupt,
# whether it interrupted idle or a thread, hard part against the softirqs run
# on its way out, and the frames under each kind.
import sys, collections
us = float(sys.argv[2])
rows = []
for l in open(sys.argv[1]):
    s, w = l.rsplit(' ', 1)
    f, w = s.split(';'), int(w)
    rows.append((f, w))
idle = lambda f: 'do_idle' in f
t = sum(w for f, w in rows if not (idle(f) and not any('_irq' in x for x in f)))
kinds = [('nvme_irq', 'nvme'), ('ipi_handler', 'IPI'), ('do_handle_IPI', 'IPI'), ('arch_timer_handler_phys', 'timer'), ('arch_timer_handler_virt', 'timer'), ('xeu', 'xeu')]
tot = collections.Counter(); self_ = collections.defaultdict(collections.Counter); incl = collections.defaultdict(collections.Counter)
for f, w in rows:
    if idle(f) and not any('_irq' in x for x in f): continue
    if 'gic_handle_irq' not in f:
        tot['no interrupt'] += w; continue
    i = f.index('gic_handle_irq'); below = f[i+1:]
    kind = 'other'
    for name, k in kinds:
        if any(name == x or name in x for x in below):
            kind = k; break
    if kind == 'other' and any('irq_exit' in x or x == 'handle_softirqs' for x in below): kind = 'exit only'
    part = 'softirq' if any(x in ('handle_softirqs', '__do_softirq') for x in below) else 'hard'
    ctx = 'idle' if 'do_idle' in f[:i] else 'thread'
    tot[(kind, part, ctx)] += w
    self_[(kind, part)][f[-1]] += w
    for x in set(below): incl[(kind, part)][x] += w
p = lambda w: '%6.2f%% %6.2f us' % (100 * w / t, us * w / t)
print('busy %.1f us a WRITE' % us)
print('\nkind      part     interrupted   share of busy')
for k, w in sorted(tot.items(), key=lambda kv: -kv[1]):
    print(('%-9s %-8s %-10s ' % k if isinstance(k, tuple) else '%-30s ' % k), p(w))
agg = collections.Counter()
for k, w in tot.items():
    if isinstance(k, tuple): agg[k[:2]] += w
print('\nby kind and part')
for k, w in agg.most_common(): print('%-9s %-8s %s' % (*k, p(w)))
for k, w in agg.most_common():
    print('\n== %s %s: %s' % (*k, p(w)))
    print('-- with what it calls')
    for x, v in incl[k].most_common(22): print('  %-40s %s' % (x, p(v)))
    print('-- self')
    for x, v in self_[k].most_common(10): print('  %-40s %s' % (x, p(v)))
