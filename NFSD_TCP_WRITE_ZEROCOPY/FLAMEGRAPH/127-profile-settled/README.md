# Round 127: a profile at the new settled-iops.sh

Mike, 18 November: "take a fresh profile with the new settled-iops.sh".
`settled-iops.sh` now holds the rebalancer (`svc_rtc_rebalance_ms` 250)
and `tcp_limit_output_bytes` 1 MiB on top of round 124's stack
(`/var/tmp/svcstack-541-rebal`, sunrpc `32BA58F914BEB5F3CF491F8`).  One
`prof-srv.sh` run, t2-r1045, against t2-r1029 (round 124, 2.88 M).

## Result: t2-r1045 (18 November, 09:26 to 09:30)

2.91 M IOPS (1.46 / 1.45), p99 3.03 / 3.49 ms, server 78% busy
(17.15 us a READ), no new kernel warning.  The busy samples (idle
left out) by area, scaled to each run's busy a READ:

| Area | t2-r1029 us | t2-r1045 us | Change |
|---|---|---|---|
| Reply send (svc and TCP) | 2.94 | 3.03 | +0.09 |
| sunrpc and the workqueue, other | 2.76 | 2.83 | +0.07 |
| softirq: TCP receive (RPS backlog) | 2.57 | 2.64 | +0.07 |
| XFS, iomap, block, NVMe | 1.82 | 1.92 | +0.10 |
| Call receive (svc and TCP) | 1.80 | 1.89 | +0.09 |
| nfsd (decode, fh, file, encode) | 1.67 | 1.74 | +0.07 |
| softirq: other (irq-off time) | 1.33 | 1.16 | -0.17 |
| softirq: the card's receive poll | 0.66 | 0.53 | -0.13 |
| softirq: the card's transmit completion | 0.64 | 0.49 | -0.15 |
| softirq: TCP Small Queues | 0.40 | 0.34 | -0.06 |
| Scheduler | 0.18 | 0.22 | +0.04 |
| Other | 0.34 | 0.36 | +0.02 |
| Total | 17.11 | 17.15 | +0.04 |

The card's softirq took 0.45 us a READ less (the rebalancer leaves
fewer connections on the ring CPUs' backlog, 1 MiB fewer TSQ
deferrals), and the work items' areas each about 0.08 us more: the
same total.  `finish_task_switch` rose 0.18 to 0.21 us.

`percpu_counter_add_batch` self 1.62% (0.28 us), from
`nfsd_finish_read` 0.10, `svc_xprt_recv_call` 0.07,
`svc_rtc_after_process` 0.04, `nfsd_cache_lookup` 0.04: a per-CPU add
that costs that much is folding into its shared count.
`arch_counter_get_cntpct` (`ktime_get`) 1.62%, 0.28 us, about 0.05 us
each from `tcp_write_xmit`, `tcp_rcv_established`,
`blk_mq_start_request`, `svc_xprt_recv_call`.
