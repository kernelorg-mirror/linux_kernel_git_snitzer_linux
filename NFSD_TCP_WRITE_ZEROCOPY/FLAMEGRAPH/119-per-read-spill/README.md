# Round 119: the server's per-READ cost at 2.78 M, again

Mike, 18 November: "take a fresh profile at 2.78 M after r1005".

Round 113 (t2-r972, 2.54 M, before the file handle borrow and the spill)
put the server at about 18 us of CPU a READ: rtc work 11.3 (process 7.3,
of it proc_read 5.2: XFS read 3.0 and the fh lookup 1.9; send 3.6; recv
2.6) and softirq 6.1.  The fh borrow (round 114) should have taken most
of the 1.9, yet round 116's t2-r995 works out at 18.4 us a READ from its
busy (80% of 64 CPUs at 2.78 M).  This profile shows where the time
goes now, and what to cut next: the reply's send, softirq, the XFS read,
the receive or the rest of the processing.

    ../113-server-per-read/prof-srv.sh 1006

One run under `settled-iops.sh` as held at `28eb5f75dd587` (the spill at
weight 1, the three borrows, nconnect 12), 8 s of `perf record -a -g` at
999 Hz from the load's second 8.  A profile changes no path: no passes.
Queued after t2-r1005, the setting's confirmation run.

## Result: t2-r1006 (18 November, 06:11 to 06:15)

2.71 M IOPS (1.35 / 1.36), p99 3.46 ms, server 78% busy (busiest 96%),
no new kernel warning.  502,578 samples at 999 Hz on 64 CPUs: 1% is
0.236 us a READ; 78% busy is 18.4 us a READ, as round 116's t2-r995.
The profile is `/dev/shm/zc-p113-1006.data` (gone at reboot).

| Stage (with children) | t2-r1006 | us a READ | t2-r972 (round 113) |
|---|---|---|---|
| rtc work items (`svc_rtc_fast`) | 46.1% | 10.9 | 11.3 |
| - processing (`svc_process`) | 27.7% | 6.5 | 7.3 |
| -- `nfsd3_proc_read` | 19.3% | 4.6 | 5.2 |
| --- the XFS read (`xfs_file_read_iter`) | 12.4% | 2.9 | 3.0 |
| --- the file (`nfsd_file_lookup_cached`) | 5.5% | 1.3 | 1.9 (7.7%) |
| -- the reply's encode (`nfs3svc_encode_readres`) | 3.0% | 0.7 | |
| - the reply's send (`tcp_sendmsg_locked`) | 14.9% | 3.5 | 3.6 |
| - the call's receive (`svc_tcp_recvfrom`) | 11.5% | 2.7 | 2.6 |
| softirq (`handle_softirqs`) | 25.0% | 5.9 | 6.1 |

The fh borrow's saving is there, about 0.6 us in the file lookup, and
processing fell 0.8 us; but the spill moved work onto the ring CPUs and
the host is busier on the whole, so the sum a READ is unchanged within
the arithmetic's noise.

### What is left, by leaf (`perf script`, callers)

- **post-op attributes: 2.5%, 0.59 us.**  `__svcxdr_encode_post_op_attr`
  is 12,531 of the encode's 14,421 samples: every READ reply fetches the
  file's attributes again (`fh_getattr`).
- **the file handle: 3.2%, 0.76 us** under `nfsd_set_fh_dentry`:
  `nfsd_fh_borrow` 4,199 (its scan of 48 entries, `memcmp`),
  `nfsd_exp_borrow` 4,074, `lockref_get` 2,340 (the dget of a hit).
- **XFS's ilock taken twice a READ: 1.8%, 0.43 us**: `down_read_trylock`
  and `__up_read` from `xfs_file_dio_read` and again from
  `xfs_read_iomap_begin`, a shared rwsem's line moving between the CPUs
  that read one file.
- **the rqst's set-up: 1.3%, 0.3 us**: `svc_generic_init_request` 5,842
  from `nfsd_init_request`, and its memset.
- **TCP's timers: 0.93%, 0.22 us**: `mod_timer` from `tcp_rearm_rto`
  2,806 and from the tail-loss probe 1,887, on each send and ACK.
- **clock reads: 1.4%, 0.33 us**: `ktime_get` from TCP's receive and
  send, `svc_xprt_recv_call`, and the block layer's accounting and
  request start (2,525).
- **the work items' park and unpark: 2.4%, 0.56 us** self, and
  `finish_task_switch` 1.9%.
- **nfsd threads still woken: `wake_up_process` from
  `svc_pool_hand_idle_thread` 2,170**, from `svc_xprt_enqueue_thread` in
  `svc_rtc_work`: some calls still go to a thread.
- `percpu_counter_add_batch` 1.0%: `nfsd_finish_read`'s read-bytes
  stats 2,307, `svc_xprt_recv_call` 1,209, the DRC's 919.
