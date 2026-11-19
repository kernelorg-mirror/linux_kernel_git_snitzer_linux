# Round 160: where the server's 14.5 us of CPU a 4 KiB READ goes

Mike, 19 November: "now profile where the 14.5 us per READ goes".  Round
159 left the server CPU-bound at 14.46 to 14.85 us a READ (mpstat busy
times CPUs over IOPS), every nfsd CPU 98% busy with two connections
each.  `profile.sh 1231`: one settled-iops.sh run of nfs4k.sh at
iodepth 96 on round 159's stack, 8 s of `perf record -a -g` at 999 Hz
from the load's eighth second, the READs counted around it.
`stages.awk` charges each sample to one stage, the first frame from the
leaf up that it names (a memcpy under `tcp_sendmsg` is the send's, a
softirq taken inside `tcp_sendmsg` is the softirq's), leaves idle out,
and gives us a READ for nfsd's CPUs (8-55) and the rings' (0-7, 56-63).
Round 150 (t2-r1199, on the stack before the spill's removal and both
rtc fixes) is the comparison; its profile can no longer be read for
sunrpc and nfsd symbols, the modules having been swapped since.

## t2-r1231 (19 November, 22:32)

3.33 M IOPS (ports 1.79 / 1.55 M), server 80% busy: the profile itself
costs some 5% (round 159's untraced runs: 3.49 to 3.57 M).  The READs
were counted over 10.68 s, perf's start and finish with the 8 s it
sampled, so its absolute us (13.62 a READ) are not the run's; and
cycles sampled at a set frequency are not time.  So the shares are
what counts, set here against round 159's 14.70 us a READ (the mean of
t2-r1226 to r1229).  `profile.sh` now counts the READs over its own 8 s.

| group | stages | share | us a READ |
|---|---|---|---|
| **network** | TCP/IP send 15.3, TCP/IP receive 9.0, softirq (other) 7.8, skb and page free 6.8, xeu 6.1, TCP ACK 1.7, IRQ 0.3 | 47.0% | 6.9 |
| **sunrpc** | rtc and dispatch 10.4, RPC receive (socket) 9.3, workqueue 1.5, RPC send 0.7 | 21.9% | 3.2 |
| **nfsd** | READ and XDR 10.3, auth and creds 3.6, fh_verify, export, DRC 3.5 | 17.4% | 2.6 |
| **storage** | XFS 7.3, block, NVMe, iomap 6.0 | 13.3% | 2.0 |

- **nfsd's CPUs carry 81% of it, the rings' 19%.**  The rings' share is
  the NIC (xeu RX and TX poll), RFS's hand-off of each packet to nfsd's
  CPUs (`enqueue_to_backlog`, `__smp_call_single_queue`,
  `gic_ipi_send_mask`: 0.4 us), and transmit completions
  (`tcp_wfree`, `skb_release_data`).
- **The rtc work item is 94% of nfsd's CPUs** (`svc_rtc_work`, 75.9% of
  every sample): `svc_process_common` 31.6% (`nfsd3_proc_read` 24.4%,
  of it `xfs_file_dio_read` 14.6% and `fh_verify` 5.9%), the call's
  receive 11.0%, the reply's `tcp_sendmsg` 13.8%, and softirqs taken
  inside it.
- **The hot leaves on nfsd's CPUs** (us a READ as sampled):
  `_raw_spin_lock` 0.29, `percpu_counter_add_batch` 0.23,
  `arch_counter_get_cntpct` (ktime) 0.20, XFS's inode lock
  (`down_read_trylock` 0.18, `__up_read` 0.18), `xfs_iext_lookup_extent`
  0.17, `nfsd_fh_borrow` 0.16 and `nfsd_exp_borrow` 0.11,
  `blk_mq_submit_bio` 0.11, `nfsd_file_lookup_locked` 0.10, memcpy and
  memset 0.10 each, `lockref_get` 0.09.
- **Without pseudo-NMI** (this boot has none) a sample taken with
  interrupts off lands where they come back on: `handle_softirqs`
  (0.33 nfsd, 0.45 rings), `_raw_spin_unlock_irq` and
  `_raw_spin_unlock_irqrestore` are where softirq and locked work end,
  not their own cost, so the leaves inside softirqs are understated.
  The stage table walks the whole stack and holds up better than the
  leaves.

Against round 150 (t2-r1199, 15.8 us, spill on, neither rtc fix): the
stages have the same order (softirq 4.6, the READ 4.3, `tcp_sendmsg`
2.6, the receive 2.1 there), and the us a READ have come down by 1.1.
