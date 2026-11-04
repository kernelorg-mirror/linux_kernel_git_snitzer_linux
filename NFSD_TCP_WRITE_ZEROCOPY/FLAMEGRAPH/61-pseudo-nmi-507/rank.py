#!/usr/bin/python3 -I
# rank.py ALLFOLDED WRITES [FRAME...]: a profile's cycles a WRITE on one group
# of CPUs, read as round 48 read fl48. ALLFOLDED is stackcollapse-perf.pl's
# output for those CPUs with the idle stacks kept (flame.sh's .folded drop
# them); WRITES is the WRITEs in the profile's window (WRITEs a second times
# its 8 s). Prints threads, hard interrupts by handler and the idle task, then
# each FRAME (or a default list) inclusive, split thread / interrupt / idle.
import sys, collections
rows = []
for l in open(sys.argv[1]):
    s, w = l.rstrip('\n').rsplit(' ', 1)
    rows.append((s.split(';'), int(w)))
nw = float(sys.argv[2])
k = lambda w: '%8.2fk' % (w / nw / 1e3)
kinds = [('nvme_irq', 'nvme_irq'), ('ipi_handler', 'IPI'), ('do_handle_IPI', 'IPI'),
         ('arch_timer_handler', 'timer'), ('xeu', 'xeu')]
cls = collections.Counter(); irq = collections.Counter(); idlex = collections.Counter()
def ctx(f):
    # an interrupt is el1_interrupt (or el0's) on the stack; the softirqs it
    # runs on its way out (irq_exit_rcu) are after gic_handle_irq returned,
    # on the interrupt's own stack, and are classed apart
    e = [i for i, x in enumerate(f) if x in ('el1_interrupt', 'el0_interrupt', 'el1h_64_irq', 'el0t_64_irq')]
    if e:
        i = e[0]; on = 'idle' if 'do_idle' in f[:i] else 'thread'
        if 'irq_exit_rcu' in f[i:]: return 'softirq', 'softirq on exit', on
        below = f[f.index('gic_handle_irq') + 1:] if 'gic_handle_irq' in f else []
        kind = 'GIC entry only'
        for name, kk in kinds:
            if any(name in x for x in below): kind = kk; break
        return 'irq', kind, on
    return ('idle', None, None) if 'do_idle' in f else ('thread', None, None)
for f, w in rows:
    c, kind, on = ctx(f)
    cls[c] += w
    if c in ('irq', 'softirq'): irq[(kind, on)] += w
    if c == 'idle': idlex['the wait' if f[-1] in ('cpu_do_idle', 'default_idle_call', 'arch_cpu_idle', 'cpuidle_idle_call') else 'rest'] += w
tot = sum(cls.values())
print('WRITEs %.0f, all %s cycles a WRITE' % (nw, k(tot)))
for c in ('thread', 'irq', 'softirq', 'idle'):
    print('  %-28s %s %5.1f%%' % (c, k(cls[c]), 100 * cls[c] / tot))
print('    idle: the wait itself %s, the rest %s' % (k(idlex['the wait']), k(idlex['rest'])))
agg = collections.Counter()
for (kind, on), w in irq.items(): agg[kind] += w
print('interrupts by handler, the softirqs on exit apart (on idle, on a thread)')
for kind, w in agg.most_common():
    print('  %-28s %s  (%s, %s)' % (kind, k(w), k(irq[(kind, 'idle')]), k(irq[(kind, 'thread')])))
frames = sys.argv[3:] or ['__schedule', 'try_to_wake_up', 'sched_ttwu_pending', 'sched_balance_newidle',
    'psi_task_switch', 'finish_task_switch', '_raw_spin_unlock_irqrestore', '_raw_spin_unlock_irq',
    'page_pool_return_netmem_bulk', '__page_pool_rx_region_put', 'page_pool_recycle_ring_bulk',
    'dma_pool_alloc', 'dma_pool_free', 'blk_mq_end_request_batch', 'iomap_dio_bio_end_io',
    'nvme_poll_cq', 'nvme_unmap_data', 'blk_rq_dma_map_iter_next', 'blk_map_iter_next',
    '__flush_workqueue', 'xlog_cil_push_now', 'nfsd3_proc_commit', 'nfsd3_proc_write',
    'hrtimer_interrupt', 'hrtimer_start_range_ns', 'napi_watchdog', 'net_rx_action',
    'gro_receive_skb', 'tcp_v4_rcv', 'handle_softirqs', 'svc_tcp_recvfrom', 'svc_recv',
    '__gic_handle_irq_from_irqson', '__gic_handle_irq_from_irqsoff']
print('\nframe inclusive                 all       thread    in irq    irq on idle   (in irq: hard and softirq on exit)')
for fr in frames:
    a = collections.Counter()
    for f, w in rows:
        if fr not in f: continue
        c, kind, on = ctx(f)
        a['all'] += w; a[c] += w
        if c == 'softirq': a['irq'] += w
        if c in ('irq', 'softirq') and on == 'idle': a['irqidle'] += w
    if a['all']:
        print('  %-28s %s %s %s %s' % (fr, k(a['all']), k(a['thread']), k(a['irq']), k(a['irqidle'])))
