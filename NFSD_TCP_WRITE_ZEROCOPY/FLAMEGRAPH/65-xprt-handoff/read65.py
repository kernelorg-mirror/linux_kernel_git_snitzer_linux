#!/usr/bin/python3 -I
# read65.py RUN...: round 65's runs side by side: GB/s, nfsd's cycles a
# WRITE (sampled, the idle loop out), the lock's fast and slow paths, the
# pool's own functions, and pool_stats' enqueues and wakes a WRITE.
import collections, os, re, sys
L = '/var/tmp/zc-run-logs'
IDLE = set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
SYMS = ['_raw_spin_lock', 'queued_spin_lock_slowpath', '__lwq_dequeue', 'svc_recv',
        'svc_xprt_enqueue', 'svc_pool_enqueue_xprt', 'svc_pool_wake_idle_thread',
        'svc_pool_hand_idle_thread', 'svc_xprt_received', 'svc_tcp_restore_pages',
        'svc_tcp_save_pages']
print('%-8s %-2s %6s %7s %9s ' % ('run', 'v', 'GB/s', 'WRITE/s', 'nfsd cyc') +
      ' '.join('%9s' % s[:9] for s in SYMS) + '   enq/W  woken/W')
for r in sys.argv[1:]:
    d = '%s/%s' % (L, r)
    log = open(d + '/pmu.log').read()
    gb = re.search(r'^total ([0-9.]+)', log, re.M).group(1)
    w = int(re.search(r'WRITEs ([0-9]+)', log).group(1))
    tag = open(d + '/results.tsv').read().split('\n')[1].split('\t')[0][-1]
    c = collections.Counter()
    for l in open(d + '/pmu.script'):
        f = l.split()
        if len(f) < 4 or not f[1].startswith('cycles'):
            continue
        if f[3] in IDLE:
            continue
        p = int(f[0]); c['(all)'] += p
        if f[3] in SYMS:
            c[f[3]] += p
    W = w * 8.0
    p0 = open(d + '/pool_stats.0').read().split('\n')[1].split()
    p1 = open(d + '/pool_stats.1').read().split('\n')[1].split()
    nw = None
    # the WRITEs over the whole run, from nfsd's counters in the log if there
    m = re.search(r'in the run admitted ([0-9]+)', log)  # loans admitted: one a WRITE
    enq = (int(p1[2]) - int(p0[2])); wok = (int(p1[3]) - int(p0[3]))
    # pool_stats covers the whole run, warm included: scale by the run's WRITEs
    runw = float(m.group(1)) if m else None
    print('%-8s %-2s %6s %7d %9.0f ' % (r, tag, gb, w, c['(all)'] / W) +
          ' '.join('%9.0f' % (c[s] / W) for s in SYMS) +
          ('   %5.2f  %6.2f' % (enq / runw, wok / runw) if runw else '   enq %d woken %d' % (enq, wok)))
