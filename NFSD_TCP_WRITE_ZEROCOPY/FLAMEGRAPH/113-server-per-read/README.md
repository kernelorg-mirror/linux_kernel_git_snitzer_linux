# Round 113: the server's per-READ cost at 2.54 M IOPS

Mike, 18 November: "profile the server at 2.54 M with both borrows".

## Why

Round 112's A/B left 4 KiB READs at 2.54 to 2.55 M IOPS with the
client's pool lock gone (scope `cpu`), and the server's busiest work
CPU at 95 to 98% in every run: the server is the limit (`IOPS.md`,
"Rounds 110 to 112", TODO 1).  Round 111's t2-r963 is the last server
profile, with only the auth borrow's symbols drawn out.

## The run

On the tip's stack (`/var/tmp/svcstack-541-kunit`: sunrpc
`85537945BAC1207EDA22E45`, nfsd `DC178C9D921DD1CDFF947D7`), both
borrows held and the client at scope `cpu` by `settled-iops.sh`.
`prof-srv.sh 972`: one 4 KiB READ run and 8 s of `perf record -a -g`
at 999 Hz from the load's second 8; reports by symbol, by command, by
CPU, and what runs on the four busiest CPUs.  A profile changes no
path, so it owes no WRITE or READ pass.

## Results (t2-r972, 18 November, 03:39)

**2.54 M IOPS (1.28 / 1.26 M) under perf**, 1.20 / 4.55 ms, server 72%
busy (busiest 96%), client 67% (busiest 75%) at scope `cpu`, no new
kernel warning.  Reports in `/var/tmp/zc-run-logs/t2-r972/`, the data in
`/dev/shm/zc-p113-972.data` (505,942 samples).

### Not one hot CPU: all 48 work CPUs

mpstat over the run: **the 48 work CPUs (8 to 55) are 89.4% busy on the
mean, 77 to 96%** (74% system, 15% softirq); the 16 ring CPUs (0 to 7,
56 to 63) 23.6% (20% softirq, at most 44%).  Each work CPU's samples are
about 94% rtc kworkers, its top symbols the host's.  The busiest at 96%
is the top of an even spread: the limit is the work CPUs' sum, the cost
of a READ times 2.54 M, not a hot spot.  No symbol is above 4.2% self.

### A READ's cost: about 18 us of CPU

At 999 Hz on 64 CPUs and 2.54 M READs a second, 1% of the samples is
about 0.25 us a READ; the host's 72% busy is about 18 us a READ.

| Stage (with children) | Samples | us a READ |
|---|---|---|
| rtc work items (`svc_rtc_fast`): receive, process, send | 44.9% | 11.3 |
| - processing (`svc_process`) | 28.9% | 7.3 |
| -- `nfsd3_proc_read` | 20.8% | 5.2 |
| --- the XFS read (`xfs_file_read_iter`; `submit_bio_noacct` 5.4%) | 11.9% | 3.0 |
| --- the file handle and file (`nfsd_file_lookup_cached`, `fh_verify` in it) | 7.7% | 1.9 |
| - the reply's send (`tcp_sendmsg_locked`) | 14.2% | 3.6 |
| - the call's receive (`svc_tcp_recvfrom`) | 10.5% | 2.6 |
| softirq (`handle_softirqs`; `net_rx_action` 17.9%) | 24.3% | 6.1 |
| idle (`default_idle_call` and its callers) | 18.5% | |

### The file handle, decoded on every READ

`perf script` over the samples under `nfsd_file_lookup_cached` (34,419,
6.8%, about 1.7 us a READ), by leaf: `_raw_spin_lock` 4,485,
`nfsd_set_fh_dentry` 2,998, `nfsd_exp_borrow` 2,310, `nfsd_setuser`
2,167, `nfsd_file_lookup_locked` 1,916, `xfs_iget` 1,616, `nfsd_file_get`
1,385, `igrab` 1,327, `lockref_get` 1,176, `__radix_tree_lookup` 864,
`memcmp` 778, `xfs_iget_cache_hit` 590, `xfs_verify_ino` 584.

The spin lock's callers: **`xfs_iget` from `xfs_nfs_get_inode` 2,184 and
`d_obtain_alias` from `xfs_fs_fh_to_dentry` 1,407**, then
`xfs_nfs_get_inode` 475 and `nfsd_set_fh_dentry` 306: the locks of the
file's inode.  Every READ decodes its file handle back to the inode
(`exportfs_decode_fh` -> `xfs_fs_fh_to_dentry` -> `xfs_iget`, a radix
lookup, `igrab`) and to a dentry (`d_obtain_alias`, `lockref_get`), and
drops both at `fh_put()`, taking the inode's locks each way; one file's
READs come on several connections and work CPUs, so those locks move
between CPUs.  Only then is the open file looked up
(`nfsd_file_lookup_locked`), by inode.

### Where this points

- **Lend a decoded file handle's dentry**, as the export borrow lends
  its export: a thread's (or rqst's) few recent file handles, with
  their dentry held, checked by the handle's bytes and the export, so
  that a hit skips `exportfs_decode_fh()`, `xfs_iget()`,
  `d_obtain_alias()` and the puts.  Up to about 1.5 us of the 18, and the
  inode locks' movement between CPUs.
- The XFS read (3.0 us), the send (3.6) and receive (2.6), and the
  softirq (6.1) are the larger shares, each spread over many symbols.
- The ring CPUs are 76% idle: running nfsd's work on them too
  (`svc_rtc_cpus`) is capacity, not cost, and settled.sh keeps them for
  the rings for WRITEs; for IOPS it is an A/B (`EXP_RTC_CPUS`).
