# Round 137: a profile of 4 KiB READs with the per-CPU choice reverted

Mike, 18 November: "profile an IOPS pass with iodepth of 96 so we can
decide on the next round to work on".  The client on its `updates/`
stack (the tip with round 136's reverts, sunrpc
`2355B960B6A9951899AF6B4`), the server on the tip's svc stack, both at
the settled setting.  `prof-both137.sh 1128`: one `settled-iops.sh` run
of `nfs4k.sh 1128:96` with 8 s of `perf record -a -g` at 999 Hz on both
hosts from the load's eighth second (the server's with `-N` and in
`/dev/shm`: nvme8 is fragile).

Before it, at iodepth 96: 3.51 to 3.63 M IOPS (t2-r1120, r1124, r1125);
at 128: 3.65 and 3.56 M (t2-r1126, r1127), both hosts' busiest CPUs at
93 to 100%.

## The profile (t2-r1128)

**3.59 M IOPS** (1.81 / 1.77 M), completion mean 1.47 / 1.51 ms, p99
3.29 / 3.29, no new kernel warning.  15.33 us of the server's CPU and
11.63 us of the client's a READ.

| Host | CPUs busy: min / median / max | Mean sys | Who |
|---|---|---|---|
| client (48) | 85 / 93 / 99% | 73% | fio 21% of samples (its submission in the kernel), rpciod workers 0.2 to 0.3% each |
| server (64) | 56 / 92 / 99% | 62% | 50 svc kworkers about 1.7% each, idle 5.7% |

**The client is the limit**: every one of its CPUs is 85% busy or more,
while some of the server's run at 56%.  One percent of the client's
samples is 0.134 us a READ (48 CPUs, 3.59 M a second).

The client's largest costs (`cli-self.txt`; `cpu_idle_poll` 11.15% is
its idle loop):

| Cost | Share | us a READ |
|---|---|---|
| spin locks, by the lock's caller: `release_sock` (the socket lock) 1.61%, `xprt_dynamic_alloc_slot` (the slot table) 1.33%, `sch_direct_xmit` (the qdisc) 0.70%, `lock_timer_base` 0.56%, `rpc_count_iostats_metrics` 0.52%, `nfs_io_size` 0.44%, `xprt_request_enqueue_receive` 0.32% | about 7% in all | about 0.9 |
| the round-robin choice: `xprt_switch_find_next_entry` 1.35%, `xprt_iter_get_next` 0.61%, `xprt_get` 0.44% | 2.4% | 0.32 |
| `memcpy` 2.67%, `_copy_to_iter` 0.48% | 3.2% | 0.42 |
| `rpc_count_iostats_metrics` (self) | 2.17% | 0.29 |
| `xprt_request_enqueue_receive` (self) | 2.05% | 0.27 |
| per-READ setup: `nfs_fattr_init` 1.75%, `rpcauth_refreshcred` 1.62%, `rpc_prepare_reply_pages` 1.37%, `nfs_pgheader_init` 1.27%, `nfs_refresh_inode_locked` 1.03% | 7.0% | 0.94 |

The server's costs are spread thin as in round 133 (`handle_softirqs`
3.97%, locks under 1% each: `xfs_read_iomap_begin`'s 0.63%,
`__dev_xmit_skb`'s 0.51%, `svc_rtc_fast`'s 0.42%).
