# Round 76: the baseline on the canary from `3bdeea5a21cc`

The canary's second boot (9 November, `7.3.3-13.hs.519.canary`, both
ports up, kdump armed, nothing in `updates/`, the settled setting, no
pseudo-NMI): its own sunrpc and nfsd, nothing swapped in, three two-port
loan runs as round 74's ([run323.sh](run323.sh)), `t2-r323..r325`.  The
held mark off, as in rounds 74 and 75, so the region figures are the
20 ms sample.

`t2-r323` stopped the script on a false warning: the canary prints
"Automounting of tracing to debugfs is deprecated" at perf's first mount
of tracing, and the check's `grep -i BUG` matched "debugfs".  Nothing
else in its dmesg was new, so the run counts; the check now skips that
line and `t2-r324` and `t2-r325` ran on it ([run323-first.out](run323-first.out),
[run323.out](run323.out)).

## The runs, beside round 75's PL on build 514

| run | | GB/s | nfsd | `svc_recv` | enq/W | woken/W | far (a, b) | region median, p99, max sampled |
|---|---|---|---|---|---|---|---|---|
| `t2-r318` | PL 514 | 70.00 | 245.7k | 2496 | 9.20 | 5.86 | 0.9%, 1.2% | 3308, 6458, 7463 |
| `t2-r320` | PL 514 | 71.51 | 249.5k | 2685 | 9.00 | 5.68 | 1.3%, 0.8% | 3424, 6783, 9402 |
| `t2-r322` | PL 514 | 70.42 | 248.7k | 3120 | 9.16 | 5.80 | 2.6%, 1.7% | 3324, 6595, 8748 |
| `t2-r323` | canary | 68.91 | 230.6k | 1847 | 9.15 | 5.92 | 2.0%, 2.3% | 3229, 6467, 11881 |
| `t2-r324` | canary | 70.37 | 238.8k | 2034 | 9.12 | 5.74 | 0.8%, 2.0% | 3341, 6841, 9627 |
| `t2-r325` | canary | 70.73 | 241.9k | 2163 | 9.11 | 5.71 | 1.5%, 1.5% | 3331, 6533, 11781 |

- **Rate: the same.**  70.00 GB/s on average against PL's 70.64, inside
  the spread of rounds 74 and 75 (69.39 to 71.51 for PF and PL).  The
  slowest, `t2-r323`, had the highest far share again.  Retransmits
  1383 to 3023, as before (1339 to 2101).
- **nfsd cycles a WRITE: 237.1k against 248.0k, 4.4% fewer.**  The fall
  is spread over every area of the profile at about the same share
  ([groups.py](groups.py), [groups-canary.txt](groups-canary.txt),
  [groups-PL.txt](groups-PL.txt)), and no function explains it
  ([symdiff-PL-canary.txt](symdiff-PL-canary.txt): the largest are
  `svc_recv()` -752 and `_raw_spin_lock` -619).  Some of PL's are
  misnamed samples of a swapped-in module (`cleanup_module` 750,
  `sunrpc_debugfs_exit` 515), which the canary's own stack does not
  have.  So the canary is no worse, and nothing in it is to be credited
  with the 4% until a run says why.
- **The queue: as PL.**  Enqueues and wakes a WRITE 9.13 and 5.79.
- **The region: median and 99th percentile as PL's; the sampled maximum
  higher** (9.6k to 11.9k against 7.5k to 9.4k).  That maximum is one
  20 ms sample and a lower bound of the peak ("What twoport reports of
  the region" in `xeu-rig/README.md`), so it does not show a rise by
  itself, but it does not rule one out: one run with
  `page_pool.rx_region_held_mark=Y` gives the exact peak to set beside
  514's (about 7,600).

## Where the canary's cycles are ([top-canary.txt](top-canary.txt))

| area | cycles a WRITE | share |
|---|---|---|
| skid at irq enable (`finish_task_switch`, `_raw_spin_unlock_irq*`) | 37.4k | 15.8% |
| **loan bookkeeping** (`rx_loan_*`, `svc_tcp_rx_*`, save and restore) | **27.8k** | **11.7%** |
| TCP/IP send and receive | 26.6k | 11.2% |
| block mapping and submit | 21.8k | 9.2% |
| spinlocks (lock and slow path) | 13.0k | 5.5% |
| svc queue and threads | 12.3k | 5.2% |
| region and page_pool | 11.4k | 4.8% |
| nfsd, XDR, caches | 10.2k | 4.3% |
| everything else | 61.6k | 26.0% |

Without pseudo-NMI the 37k at irq enable is time spent with interrupts
masked, credited where they are unmasked; round 61 has where it goes.

The loan's own bookkeeping is the largest block of the series' own code,
and it runs at high IPC with few refills, so it is instructions, not
waits: `__rx_loan_table_append_ref` 4.3k, `svc_tcp_rx_borrow_segment`
3.4k, `rx_loan_table_release` 3.1k, `rx_loan_prefetch_pages` 3.1k,
`svc_tcp_rx_walk_skb` 2.7k, `svc_tcp_rx_validate_mixed` 1.6k,
`rx_loan_region_span` 1.6k, `rx_loan_skb_region_pages` 1.2k,
`svc_tcp_rx_append_ref` 1.1k.  Each skb's frags are walked by several of
them in turn.  Next is `blk_rq_dma_map_iter_next` at 5.0k (IPC 4.2),
the walk of a loaned WRITE's bvecs into DMA segments, one a region page.

## Suggested focus for round 77

**The loan's per-segment bookkeeping: walk each received skb's frags
once.**  The walk, the borrow, the table append and the span check each
visit every frag of a loaned record: about 28k cycles a WRITE, 12% of
nfsd's, and none of it touches more of the region (fewer passes over the
same pages, nothing held longer), so it does not grow the footprint.
Before code: one profile with call graphs (`perf record -g`) on these
CPUs, to see which pass calls which and how many frags each visits per
WRITE, and one run with the held mark on, for the exact peak.

After that, the DMA map walk (5.0k): adjacent pages of the region are
adjacent in physical memory, so a loaned WRITE's segments could merge
before `blk_rq_dma_map_iter_next()` maps them one page at a time.  It is
block code, so it would be a change for its own sake, not the loan's.
The spinlocks (8.3k on `_raw_spin_lock`, 1.6k in the slow path) also
need the call graph before anything can be said of them.
