# Round 130: the server and the client at 3 M

Mike, 18 November: "take a fresh profile at 3.07 M (ensure the
temporary module retains the switch at Y), also profile the client
because yes it does seem like the client is the limit now".

tardis2 runs round 129's stack (`/var/tmp/svcstack-541-partial`,
sunrpc `99424F2CE6E026FE151DCE7`) with `svc_rtc_partial_wait_ab` at Y,
read before and after the run.  `prof-both.sh 1052`: one
`settled-iops.sh` run, 8 s of `perf record -a -g` at 999 Hz on both
hosts together from the load's eighth second.  Set against t2-r1045
(round 127, 2.91 M, server only).

## Result: t2-r1052 (18 November, 10:15 to 10:20)

3.06 M IOPS (1.52 / 1.54), p99 2.51 / 2.54 ms, server 76% busy (busiest
100%), client 77% (busiest 83%), no new kernel warning.  The switch read
Y before and after; 8,564,985 partial waits and 4 hand-offs to threads
in the run.

### The server: 15.90 us a READ (t2-r1045 17.15)

| Area | t2-r1045 | t2-r1052 | Change |
|---|---|---|---|
| Reply send (svc and TCP) | 3.03 | 2.89 | -0.14 |
| sunrpc and the workqueue, other | 2.83 | 2.56 | -0.26 |
| softirq: TCP receive (RPS backlog) | 2.64 | 2.53 | -0.11 |
| XFS, iomap, block, NVMe | 1.92 | 1.81 | -0.11 |
| Call receive (svc and TCP) | 1.89 | 1.74 | -0.14 |
| nfsd | 1.74 | 1.58 | -0.16 |
| softirq: other (irq-off time) | 1.16 | 1.06 | -0.09 |
| softirq: the card's receive, transmit, TSQ | 1.36 | 1.36 | 0 |
| Scheduler and other | 0.58 | 0.36 | -0.22 |

The work CPUs' areas all fell by 0.1 to 0.26 us (`finish_task_switch`
0.21 to 0.10, `_raw_spin_unlock_irqrestore` 0.43 to 0.30); the card's
softirq is unchanged.

### The client: 12.08 us a READ (48 CPUs at 77%)

By the samples outside `do_idle` (78.6%, mpstat 77%), inlined frames
left out:

| Area | Share | us a READ |
|---|---|---|
| RPC state machine (`__rpc_execute`: start, reserve, refresh, encode, decode, exit) | 24.5% | 2.96 |
| NFS client (direct read setup, completion) | 14.0% | 1.69 |
| RPC receive (`xs_read_stream`, `tcp_recvmsg`) | 11.9% | 1.43 |
| RPC send (`xprt_transmit`, `tcp_sendmsg`) | 11.6% | 1.40 |
| Workqueue and scheduler | 11.4% | 1.38 |
| softirq: TCP/IP receive | 6.1% | 0.73 |
| softirq: the NIC's poll | 5.7% | 0.69 |
| fio | 5.1% | 0.62 |
| io_uring | 3.7% | 0.44 |
| softirq other, interrupts, other | 6.0% | 0.73 |

Self: `_raw_spin_lock` 0.91 us (`xprt_alloc_slot`'s `reserve_lock`
0.22, the qdisc's `sch_direct_xmit` 0.14, `rpc_count_iostats_metrics`
0.07, `nfs_direct_read_completion` 0.05), `call_start` 0.31 (57% on
`clnt->cl_stats->rpccnt++`, 35% on the version's `counts[]++`: two
counters every CPU writes, as round 124 found on the server),
the queued spinlock's slow path 0.29 (`process_one_work`'s pool lock
0.05 of it), `memcpy` 0.27, `rpc_count_iostats_metrics` 0.19,
`rpcauth_refreshcred` and `rpcauth_init_cred` 0.26.

Busiest client CPUs 82 to 85% by samples, least busy 71%.

### What bounds it

Neither host is at its CPUs: the server at 15.90 us a READ on 64 CPUs
would carry 4.0 M at 100%, the client at 12.08 on 48 about the same.
With 3,072 READs in flight (iodepth 64, 48 jobs) at 1.00 ms mean,
Little's Law gives the 3.07 M measured: the rate is the latency's, and
the latency is the queueing on the busiest CPUs of both hosts.
