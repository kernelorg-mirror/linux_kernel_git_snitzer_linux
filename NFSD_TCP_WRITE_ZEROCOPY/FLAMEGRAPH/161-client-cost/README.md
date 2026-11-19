# Round 161: where the client's 11.4 us of CPU a 4 KiB READ goes

Mike, 19 November: "profile the client's 11.4 us per READ too".  Round
159's untraced runs put the client at 11.29 to 11.42 us a READ (mean
11.36), 83 to 84% busy, its busiest CPU 96 to 98%.  The client is 106:
2 x Xeon Gold 6542Y (48 cores, no SMT, 120 MiB L3), 1 TiB DDR5 at
5200 MT/s, two ConnectX-7 at 400 Gb/s; it runs `idle=poll`.
`profile-client.sh 1232`: round 160's run, with 8 s of `perf record -a
-g` on the client from the load's eighth second; `cstages.awk` charges
each sample to a stage as `stages.awk` does on the server, with
user-space frames (fio, liburing) apart, and groups by command (fio,
kworker, interrupts on idle CPUs, other).

## t2-r1232 (19 November, 23:57)

3.47 M IOPS (ports 1.76 / 1.72 M), p99 3.19 / 3.23 ms, client 83% busy
(busiest 96%): 11.48 us a READ by mpstat, 11.67 from the samples (the
READs counted over the 8 s sampled; with `idle=poll` cycles are time).
The profile cost the client little: round 159's untraced runs read
3.49 to 3.57 M.  The run's "new kernel warnings 1" was the server's
dmesg wrapping (an old line fell off the front, and the check counted
removed lines too); `nfs4k.sh` counts added lines only now.

| group | stages (us a READ) | us a READ | share |
|---|---|---|---|
| **network** | TCP/IP receive 1.27, TCP/IP send 0.99, mlx5 0.70, softirq 0.25, skb free 0.19, TCP ACK 0.16, IRQ 0.13 | 3.69 | 32% |
| **sunrpc client** | `xprt_*`, `xs_*`, `call_*`, `rpc_*` | 3.64 | 31% |
| **scheduler and workqueue** | wakeups 1.03, workqueue 0.68 | 1.71 | 15% |
| **NFS client** | `nfs_*` | 1.49 | 13% |
| **fio's own side** | user space 0.50, io_uring 0.40, syscall entry and exit 0.20 | 1.10 | 9% |

- **By command: the kworkers (rpciod, xprtiod) 8.32 us (71%), fio 2.71
  (23%)**, interrupts on idle CPUs 0.32, the rest 0.32.  fio's 2.71 is
  its submission: io_uring, `nfs_file_direct_read` and the RPC's
  start, of it `nfs_fattr_init` 0.20 and `nfs_pgheader_init` 0.19
  (structures zeroed for each READ) and
  `xprt_switch_find_next_entry_roundrobin` 0.14 (nconnect's choice of
  transport).
- **Spin locks are 1.88 us a READ (16%)**: `_raw_spin_lock` 1.01,
  `native_queued_spin_lock_slowpath` (contended) 0.37, `_bh` 0.26,
  `_irqsave` 0.24.  By caller, most samples first:
  - `_raw_spin_lock`: `xprt_alloc_slot` (the transport's
    `reserve_lock`), `sch_direct_xmit` (the qdisc's lock),
    `rpc_count_iostats_metrics` (the mount's per-procedure counters, one
    lock for every CPU), `xprt_request_enqueue_receive` (the transport's
    `queue_lock`), `nfs_direct_read_completion`, `__queue_work` (the
    workqueue pool's lock);
  - contended: `xprt_request_enqueue_transmit`, `process_one_work`
    (the pool lock, IOPS.md's), `xprt_request_enqueue_receive`, the run
    queue's lock, `xs_read_stream`, `xprt_request_transmit`;
  - `_bh`: `release_sock` (the socket's lock);
  - `_irqsave`: `lock_timer_base` (timers), `try_to_wake_up`,
    `complete`.
- **Other hot leaves:** `memcpy` 0.40 (the reply's 4 KiB into fio's
  buffer, the one copy a READ must make), `xprt_request_enqueue_receive`
  0.37 of its own (the transport's receive tree), `lock_sock_nested`
  0.15, `nfs_refresh_inode_locked` 0.12, `__rpc_execute` 0.11,
  `xprt_lookup_rqst` 0.11, `fget` 0.10, `ktime_get` 0.10.

Against the server (round 160, 14.7 us): the client spends as much on
sunrpc as on its network stack, the server half as much; and a sixth of
the client's CPU is spin locks of the transport, the mount and the
workqueue pool, against no lock above 0.3 us on the server.
