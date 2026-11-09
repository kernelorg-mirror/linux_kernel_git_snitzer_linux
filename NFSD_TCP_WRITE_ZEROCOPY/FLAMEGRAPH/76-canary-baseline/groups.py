#!/usr/bin/python3 -I
# groups.py RUN...: cycles a WRITE by area (nfsd's CPUs, idle out), mean.
import collections, lzma, re, sys
L='/var/tmp/zc-run-logs'
IDLE=set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
G=[('skid at irq enable', r'^(finish_task_switch|_raw_spin_unlock_irq|_raw_spin_unlock_bh$)'),
   ('loan bookkeeping', r'^(__?rx_loan|rx_loan|svc_tcp_rx_|svc_tcp_(save|restore)_pages)'),
   ('region/page_pool', r'^(rx_region|page_pool|napi_pp_put_page|__page_pool)'),
   ('block mapping/submit', r'^(blk_|bio_|__blk|dma_direct|nvme_|dma_)'),
   ('spinlocks', r'^(_raw_spin_lock|queued_spin_lock_slowpath|_raw_spin_unlock$)'),
   ('svc queue/threads', r'^(svc_recv|svc_xprt|svc_pool|__lwq|svc_thread)'),
   ('tcp/ip tx+rx', r'^(tcp_|__tcp|ip_|__ip|__dev_|dev_|skb_|__skb|kfree_skb|lock_sock|release_sock|__sk_|sk_)'),
   ('nfsd/xdr/cache', r'^(nfsd|svcxdr|__svcxdr|nfs3|nfs4|sunrpc_cache|rqst_exp|exp_|svc_generic)'),
   ('xfs/iomap', r'^(xfs|iomap|__iomap)'),
   ('slab/mm', r'^(kmem_|__kmem|kfree|__memcg|slab|___slab|folio|__folio|mem_cgroup)'),
   ('sched/timer', r'^(schedule|__schedule|try_to_wake|ttwu|enqueue|dequeue|update_|pick_|arch_counter|hrtimer)')]
tot=collections.Counter(); W=0
for r in sys.argv[1:]:
    W+=int(re.search(r'WRITEs ([0-9]+)',open(f'{L}/{r}/pmu.log').read()).group(1))*8.0
    for l in lzma.open(f'{L}/{r}/pmu.script.xz','rt'):
        f=l.split()
        if len(f)<4 or not f[1].startswith('cycles') or f[3] in IDLE: continue
        k='(other)'
        for n,p in G:
            if re.match(p,f[3]): k=n; break
        tot[k]+=int(f[0])
T=sum(tot.values())
for k,v in tot.most_common(): print('%7.0f %5.1f%%  %s'%(v/W,100*v/T,k))
