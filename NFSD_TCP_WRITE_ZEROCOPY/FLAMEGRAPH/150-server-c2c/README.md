# Round 150: the server's CPU a READ, by stage and by cache line

Mike, 19 November: "should we just focus on reducing CPU time please?"
then "yes, cancel round 148 and run 149, then the server breakdown
using c2c on the server".  The server spends about 15.7 us of CPU a 4
KiB READ (64 CPUs, 85 to 89% busy at about 3.6 M IOPS), where local fio
on the same drives costs about 3.4 us (18.78 M IOPS, cores-bound, round
108).  It has never had a perf c2c run; round 145's two shared lines on
the client were worth 0.55 us a READ.

Round 148 is cancelled: its script was stopped during its first run
(t2-r1185, nconnect 12, left to finish), so it has no result.

**perf c2c on tardis2 needs ARM SPE**: `perf c2c record` says "no PMU
supports the memory events"; `arm_spe_pmu.ko` is built and not loaded.
`spe-probe.sh`, through `step.sh` after t2-r1185 and before round 149:
`modprobe arm_spe_pmu`, and whether a PMU appears (the firmware must
describe SPE's interrupt in the MADT).  If none, the breakdown is by
stage from a call-graph profile only.

**No SPE** (`spe-probe.sh`, 09:40): `modprobe arm_spe_pmu` exits 0 and
no PMU appears, so the firmware does not describe SPE; `perf c2c` cannot
record on tardis2.  The module stays loaded and idle.  t2-r1185 (round
148's one run, nconnect 12, the client on `updates/`): 3.55 M IOPS.

Instead, `breakdown.sh 1199` after round 149: one IOPS run, the client
on `updates/`, and 8 s of `perf record -a -g` on the server with cycles
(the CPU by stage), `l2d_cache_refill_rd` (reads that missed the core's
L2: from another core, the L3 or memory) and `ll_cache_miss_rd` (reads
that went to memory).  Misses by function, not by line: where the cost
is misses and where it is work.

## The breakdown (t2-r1199)

3.61 M IOPS (1.86 / 1.75 M), p99 3.06 / 3.26 ms, the server 89% busy:
**15.78 us of the server's CPU a READ**, 1% of its cycles 0.158 us.  The
client on its new `updates/` stack (round 149's).  Samples: cycles 517K,
`l2d_cache_refill_rd` 609K, `ll_cache_miss_rd` 926K.

By stage, inclusive (`srv-cycles-children.txt`; the stages overlap where
softirq runs inside a send):

| Stage | Share | us a READ |
|---|---|---|
| softirq (`handle_softirqs`): receives, ACKs, transmit completions | 29.1% | 4.6 |
| the NFS READ (`nfsd3_proc_read`) | 27.5% | 4.3 |
| of it, XFS's direct read to the drive (`xfs_file_dio_read`) | 16.2% | 2.6 |
| of it, `fh_verify` | 6.9% | 1.1 |
| the reply's send (`tcp_sendmsg`) | 16.6% | 2.6 |
| the call's receive (`svc_xprt_recv_call`) | 13.3% | 2.1 |
| `svc_authenticate` | 2.7% | 0.4 |

Local fio reads at about 3.4 us a READ (round 108), close to XFS's 2.6
here: **the network is most of what NFS adds, some 9 us of the 15.8**.

Costs whose misses outrun their cycles (shares of each event's samples),
by caller from `perf script --no-inline`:

| What | Cycles | L2 misses | LL misses | us a READ (cycles) |
|---|---|---|---|---|
| XFS's inode locks, shared on one file by every CPU: `down_read_trylock` from `xfs_ilock_nowait` (dio read and `xfs_read_iomap_begin`), `__up_read` from `xfs_iunlock`, the spin lock in `xfs_read_iomap_begin` | 3.2% | 2.3% | 2.4% | 0.50 |
| `nfsd_fh_borrow` | 1.11% | **3.12%** | 2.60% | 0.18 |
| `lockref_get` from `nfsd_set_fh_dentry` (the file's dentry, every CPU) | 0.72% | 1.08% | 2.29% | 0.11 |
| `tcp_wfree` (a sent skb freed on the transmit-completion CPU) | 0.77% | 1.14% | **3.46%** | 0.12 |
| creds: `refcount_dec_not_one` from `free_uid` from `put_cred_rcu`, the shared `user_struct` | 1.07% | | | 0.17 |
| per-CPU counters (`nfsd_finish_read`, `svc_xprt_recv_call`, `svc_rtc_after_process`, `nfsd_cache_lookup`) | 1.47% | 1.5% | 1.35% | 0.23 |
| `ktime_get` (`arch_counter_get_cntpct`): block accounting, `tcp_write_xmit`, `svc_xprt_recv_call` | 1.64% | | | 0.26 |
| `mod_timer` from `sk_reset_timer` (TCP's timer on each send) | 0.89% | | | 0.14 |
| the qdisc lock (`__dev_xmit_skb`) | 0.86% | | | 0.14 |
