#!/usr/bin/python3
# pmu.py SCRIPTOUT: cycles, IPC and L1 and L2 refills by symbol, for the SGL walk (round 55).
import collections, sys
want = """blk_map_iter_next bio_split_io_at blk_rq_dma_map_iter_next blk_rq_dma_map_iter_start
dma_map_phys dma_direct_map_phys nvme_pci_setup_data_sgl nvme_map_data nvme_prep_rq.part.0
nvme_queue_rqs blk_mq_submit_bio __bio_split_to_limits bio_submit_split iomap_dio_bio_iter_one
svc_tcp_rx_append_ref svc_tcp_rx_borrow_segment svc_tcp_rx_walk_skb svc_tcp_rx_release_refs""".split()
IDLE = 'default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split()
c = collections.defaultdict(collections.Counter)
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) < 4:
        continue
    period, ev, sym = f[0], f[1].rstrip(':').split(':')[0], f[3]
    if sym in IDLE:
        continue
    try:
        c[ev][sym] += int(period)
    except ValueError:
        pass
T = {e: sum(v.values()) or 1 for e, v in c.items()}
print('events', {e: T[e] for e in T})
cy, ins, rf, r1 = 'cycles', 'instructions', 'l2d_cache_refill', 'l1d_cache_refill'
print(f"{'symbol':32} {'%cyc':>6} {'IPC':>5} {'L1 refills/kI':>13} {'L2 refills/kI':>13} {'%L2 refill':>10}")
print(f"{'ALL':32} {100.0:6.2f} {T[ins]/T[cy]:5.2f} {1000*T[r1]/T[ins]:13.1f} {1000*T[rf]/T[ins]:13.1f} {100.0:10.2f}")
for s in sorted(want, key=lambda s: -c[cy][s]):
    if c[cy][s]:
        i = c[ins][s] or 1
        print(f"{s:32} {100*c[cy][s]/T[cy]:6.2f} {c[ins][s]/c[cy][s]:5.2f} {1000*c[r1][s]/i:13.1f} {1000*c[rf][s]/i:13.1f} {100*c[rf][s]/T[rf]:10.2f}")
