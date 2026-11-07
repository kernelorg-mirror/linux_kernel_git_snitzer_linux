#!/usr/bin/python3 -I
# areas62.py A.script WRITES_A B.script WRITES_B: pmu62.py's samples summed
# by area (the loan's renamed functions on 514 grouped with 506's), in cycles
# and instructions a WRITE, B - A, the largest changes first.
import collections, re, sys
IDLE = set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
AREAS = [
 ('the loan (sunrpc rx, region)', r'^(svc_tcp_rx_|__rx_loan|rx_loan|rx_region|svc_tcp_restore_pages|svc_tcp_recv_skb|svc_tcp_recv_actor|page_pool_rx_region|__page_pool_rx_region)'),
 ('page_pool and skb frees', r'^(page_pool|napi_pp_put_page|__page_pool|skb_|kfree_skb|__kfree_skb|napi_|__napi)'),
 ('the map walk and NVMe', r'^(blk_map_iter_next|__blk_map_iter_next|blk_rq_dma_map|nvme_|dma_map_phys|dma_direct_map_phys|bio_split_io_at|dma_pool)'),
 ('block and iomap', r'^(blk_|bio_|__bio|submit_bio|__submit_bio|iomap_|__iomap)'),
 ('xfs and vfs', r'^(xfs_|xlog|vfs_|file_|generic_|iov_iter)'),
 ('TCP receive', r'^(tcp_recv|__tcp_read_sock|tcp_read_sock|tcp_cleanup|__tcp_cleanup|tcp_rcv|tcp_v4_rcv|__sk_mem|sk_mem|tcp_data|tcp_event)'),
 ('transmit (TCP, IP, qdisc, xeu)', r'^(__tcp_transmit|tcp_transmit|tcp_send|__tcp_send|tcp_write|ip_|__ip_|neigh|__dev_xmit|dev_hard|__dev_queue|sch_|__qdisc|qdisc|xeu_|skb_release|tcp_sendmsg|__tcp_push|tcp_push|validate_xmit|dev_qdisc|netif_skb|tcp_wfree|sock_wfree|__get_xps)'),
 ('locks', r'^(_raw_spin|queued_spin|_raw_read|_raw_write|mutex_|__mutex)'),
 ('sleep and wake', r'^(finish_task_switch|__schedule|schedule|try_to_wake|ttwu|wake_up|svc_thread|svc_pool_wake|svc_recv$|sched_|update_|enqueue_|dequeue_|pick_|psi_|select_task|set_next|put_prev|cpuacct|__switch_to|cpu_switch_to|account_|load_balance|newidle|idle_|tick_|hrtimer|irq|gic|handle_|el1|el0)'),
 ('sunrpc and nfsd, rest', r'^(svc_|nfsd|xdr_|sunrpc|cache_|exp_|fh_|__fh|rqst_|nfs)'),
]
def load(path, writes):
    c = collections.defaultdict(collections.Counter)
    for l in open(path):
        f = l.split()
        if len(f) < 4: continue
        try: p = int(f[0])
        except ValueError: continue
        ev, sym = f[1].rstrip(':').split(':')[0], f[3]
        if sym in IDLE: continue
        a = next((n for n, rx in AREAS if re.match(rx, sym)), 'everything else')
        c[ev][a] += p / writes; c[ev]['(all)'] += p / writes
    return c
a = load(sys.argv[1], float(sys.argv[2])); b = load(sys.argv[3], float(sys.argv[4]))
print('%-34s %9s %9s | %9s %9s | %8s %8s' % ('area, k a WRITE', 'cyc A', 'ins A', 'cyc B', 'ins B', 'd cyc', 'd ins'))
names = sorted(set(a['cycles']) | set(b['cycles']), key=lambda n: -abs(b['cycles'][n] - a['cycles'][n]))
for n in ['(all)'] + [x for x in names if x != '(all)']:
    k = lambda d, e: d[e][n] / 1e3
    print('%-34s %9.1f %9.1f | %9.1f %9.1f | %+8.1f %+8.1f' % (n, k(a,'cycles'), k(a,'instructions'), k(b,'cycles'), k(b,'instructions'), k(b,'cycles') - k(a,'cycles'), k(b,'instructions') - k(a,'instructions')))
