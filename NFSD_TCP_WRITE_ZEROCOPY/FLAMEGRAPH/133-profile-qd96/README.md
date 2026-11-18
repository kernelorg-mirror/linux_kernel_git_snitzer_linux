# Round 133: the server at iodepth 96

Mike, 18 November: "take a fresh profile of the server at iodepth 96".
Round 132 left the server the lower ceiling: at iodepth 96 (t2-r1063,
3.50 M) the server took 15.54 us of CPU a READ on 64 CPUs (4.12 M at
100%), the client 11.38 on 48 (4.22 M).

The client runs round 132's P stack (the tree at `e476bfc6bb64e`, its
calls counted per CPU), as in t2-r1063; the server round 129's stack at
the settled setting.  `prof-srv.sh 1065` (iodepth 96): one
settled-iops.sh run with 8 s of perf on the server, its data in
/dev/shm and no build-id cache, so that nothing but the reports goes to
the root disk.  The client goes back to build 541's stack after.

## Result (18 November, 16:28 to 16:32)

t2-r1065: 3.22 M (1.70 / 1.52), p99 3.72; 15.01 ms (port b's tail is
perf's: t2-r1063 without it read 3.50 M, p99 3.78), server busy 79%,
client 78% (busiest 91%), no new kernel warning.  The client was on P
(sunrpc `C4A23C35035DD3A1E1C967B`) and is back on build 541's stack;
the host at the settled setting.  perf's data is
`/dev/shm/zc-p133-1065.data`; `perf script` dumps of it and of round
130's t2-r1052 (iodepth 64, 3.06 M) were made in /dev/shm and compared
by round 127's areas (us a READ = share of all samples x 64 CPUs / IOPS,
perf's own cost in both):

| Area | r1052 (qd 64) us | r1065 (qd 96) us | Change |
|---|---|---|---|
| reply send (svc + TCP) | 3.63 | 3.40 | -0.23 |
| sunrpc and workqueue other | 3.21 | 3.05 | -0.17 |
| softirq: TCP receive (RPS backlog) | 3.17 | 2.85 | -0.32 |
| XFS, iomap, block, NVMe | 2.27 | 2.21 | -0.06 |
| call receive (svc + TCP) | 2.19 | 2.05 | -0.14 |
| nfsd (decode, fh, file, encode) | 1.98 | 1.91 | -0.07 |
| softirq: other (incl. irq-off time) | 1.33 | 1.26 | -0.07 |
| softirq: NIC transmit completion | 0.60 | 0.68 | +0.09 |
| softirq: NIC receive poll | 0.66 | 0.58 | -0.09 |
| softirq: TCP Small Queues work | 0.43 | 0.48 | +0.04 |
| other and scheduler | 0.45 | 0.41 | -0.04 |
| **busy total** | **19.93** | **18.87** | **-1.06** |

The deeper queue makes nearly every area cheaper a READ (more work for
each wakeup and each softirq pass); only the transmit side's
completions and TSQ grow.  Nothing new rose to the top: the server's
cost is spread, and the leaves worth a look, by caller
(`callers.py`):

| Leaf (self) | us | Where |
|---|---|---|
| XFS's inode locks: `down_read_trylock` + `__up_read` + the spin lock in `xfs_read_iomap_begin` | 0.56 | `xfs_ilock_nowait` from `xfs_file_dio_read` (IOLOCK) and `xfs_read_iomap_begin` (ILOCK), each shared, each an atomic on the inode's rwsem |
| `arch_counter_get_cntpct` (time stamps) | 0.34 | block accounting (`blk_account_io_start` 0.06, `blk_mq_start_request` 0.06), `svc_xprt_recv_call` 0.06, TCP send and receive 0.11 |
| `percpu_counter_add_batch` | 0.33 | `nfsd_finish_read` 0.12, `svc_xprt_recv_call` 0.08, `nfsd_cache_lookup` 0.04, rtc 0.08 |
| qdisc lock (`_raw_spin_lock` in `__dev_xmit_skb`, `__qdisc_run`, `neigh_hh_output`) | 0.22 | the codel qdisc's root lock on each transmit |
| `mod_timer` from `sk_reset_timer` (`_raw_spin_unlock_irqrestore`) | 0.19 | TCP rearming its retransmit timer on sends |
| workqueue pool lock (`_raw_spin_unlock_irq` in `worker_thread`) | 0.21 | the rtc work items' worker loop |
| `handle_softirqs` self | 0.80 | irq-off time inside softirq, spread |
