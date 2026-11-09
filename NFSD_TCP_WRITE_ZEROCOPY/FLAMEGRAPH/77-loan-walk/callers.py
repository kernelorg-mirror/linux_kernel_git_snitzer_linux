#!/usr/bin/python3 -I
# callers.py RUN [N]: from RUN's pmu-g.script.xz (cycles with call
# graphs, nfsd's CPUs), cycles a WRITE: (1) inclusive, for the receive
# path's and the loan's functions; (2) for each of the loan's per-segment
# functions and the spinlocks, its N heaviest callers (for a lock, the
# first caller that is not itself a lock function), and (3) the chains
# under svc_tcp_recvfrom() that the loan's functions are reached by.
import collections, lzma, re, sys
L = '/var/tmp/zc-run-logs'
r = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 8
IDLE = set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
LOAN = re.compile(r'^(__?rx_loan|rx_loan|svc_tcp_rx_|svc_tcp_(save|restore)_pages|xdr_buf_to_bvec)')
LOCK = re.compile(r'^(_raw_spin|queued_spin|do_raw_spin|__raw_spin)')
LEAVES = ['_raw_spin_lock', 'queued_spin_lock_slowpath', '_raw_spin_lock_bh',
          '_raw_spin_unlock_irqrestore', 'blk_rq_dma_map_iter_next']
W = int(re.search(r'WRITEs ([0-9]+)', open(f'{L}/{r}/pmu.log').read()).group(1)) * 8.0
incl = collections.Counter(); self_ = collections.Counter()
callers = collections.defaultdict(collections.Counter)
chains = collections.Counter(); total = 0
def take(period, frames):
    global total
    if not frames or frames[0] in IDLE: return
    total += period
    self_[frames[0]] += period
    for f in set(frames): incl[f] += period
    leaf = frames[0]
    if LOAN.match(leaf) or leaf in LEAVES:
        if LOCK.match(leaf):
            up = next((f for f in frames[1:] if not LOCK.match(f)), '?')
        else:
            up = frames[1] if len(frames) > 1 else '?'
        callers[leaf][up] += period
    # the path from svc_tcp_recvfrom down to a loan leaf, root first
    if LOAN.match(leaf) and 'svc_tcp_recvfrom' in frames:
        i = frames.index('svc_tcp_recvfrom')
        chains[' > '.join(reversed(frames[:i + 1]))] += period
period = None; frames = []
for l in lzma.open(f'{L}/{r}/pmu-g.script.xz', 'rt'):
    if not l.strip():
        if period is not None: take(period, frames)
        period = None; frames = []; continue
    if l[0] in ' \t' and period is not None and not re.match(r'^\s*[0-9]+\s+cycles', l):
        # perf prints the functions inlined at an address before the one
        # that holds them, all with the same address: keep the holder.
        p = l.split()
        sym = re.sub(r'\+0x[0-9a-f]+$', '', p[1]) if len(p) > 1 else '[unknown]'
        if p[-1] == '(inlined)':
            continue
        frames.append(sym)
        continue
    m = re.match(r'^\s*([0-9]+)\s+cycles', l)
    if m:
        if period is not None: take(period, frames)
        period = int(m.group(1)); frames = []
if period is not None: take(period, frames)
print('%s: %.0f cycles a WRITE (idle out)\n' % (r, total / W))
print('inclusive, the receive path and the loan:')
for k, v in sorted(incl.items(), key=lambda x: -x[1]):
    if LOAN.match(k) or k in ('svc_tcp_recvfrom', 'svc_tcp_recv_skb', 'svc_tcp_read_sock', 'tcp_read_sock',
                              'svc_recv', 'svc_handle_xprt', 'svc_process', 'nfsd_dispatch', 'nfsd_vfs_write',
                              'nfsd3_proc_write', 'nfsd4_write', 'xfs_file_write_iter', 'iomap_dio_rw',
                              'submit_bio', 'blk_mq_submit_bio', 'svc_send', 'svc_tcp_sendto', 'nfsd'):
        print('  %7.0f incl %7.0f self  %s' % (v / W, self_[k] / W, k))
print('\ncallers:')
for leaf in sorted(callers, key=lambda k: -sum(callers[k].values())):
    c = callers[leaf]; s = sum(c.values())
    if s / W < 300: continue
    print('  %-34s %6.0f' % (leaf, s / W))
    for k, v in c.most_common(N):
        print('      %6.0f  %5.1f%%  %s' % (v / W, 100 * v / s, k))
print('\nchains under svc_tcp_recvfrom to a loan leaf:')
for k, v in chains.most_common(25):
    print('  %6.0f  %s' % (v / W, k))
