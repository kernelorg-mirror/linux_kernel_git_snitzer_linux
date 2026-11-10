# Round 88: the profile on build 522, for the next round's focus

10 November, tardis2, `7.3.3-14.hs.522.canary` (built from
`d87b7e7c664b`, round 87's prefetch in), second boot, with the tip's
sunrpc from `updates/` (srcversion `F99F549CC7AF44FA39DAE34`,
`$L/sunrpc-522-tip`) and the kernel's own nfsd.

`run367.sh` checks the loaded sunrpc is the tip's instead of refusing a
non-empty `updates/`, then four two-port loan runs as round 76's:

- `t2-r367`, `t2-r368`: counted profile (cycles, instructions, L1D and
  L2D refills), for the totals and the top functions;
- `t2-r369`: cycles with call graphs, for who calls what (round 77's
  `t2-r327`);
- `t2-r370`: the held mark, for the region's exact peak.

Read with round 76's `read65.py`, `fsum.py`, `top.py`, `groups.py`,
`symdiff.py` and round 77's `callers.py`, `under.py`.

## Result: the same as round 76; the release is the largest of the loan's own

`run367.out`.  No new kernel warning in any run.

| run | kind | GB/s | WRITEs/s | nfsd cycles a WRITE | enq/W | woken/W |
|---|---|---|---|---|---|---|
| `t2-r367` | counted | 69.89 | 66,533 | 242,898 | 9.18 | 5.81 |
| `t2-r368` | counted | 70.63 | 67,315 | 244,808 | 9.07 | 5.71 |
| `t2-r369` | call graph | 69.98 | 66,520 | 238,874 | | |
| `t2-r370` | held mark | 70.86 | 67,344 | | | |
| round 76, `t2-r324`, `t2-r325` | counted | 70.37, 70.73 | | 238,847, 241,934 | 9.12, 9.11 | 5.74, 5.71 |

- **Rate and cycles as round 76** (`read65.py`), and the areas within 1k
  of it everywhere (`groups-522.txt` against `../76-canary-baseline/groups-canary.txt`).
- **Round 87's prefetch shows:** `__rx_loan_table_append_ref` 3,162
  cycles a WRITE against 4,308 in round 76 (`top-522.txt`).
- **The region's exact peak is 11,919 pages** (`t2-r370`), as round 87's
  A (11,928), above round 77's 8,092 and 87's B (9,843): the peak varies
  that much from run to run, so one run says no more than "not raised
  beyond what has been seen".
- 12.2 context switches and 4.4 threads woken a WRITE (`t2-r367`'s
  `pmu.log`).

**Where the cycles are, from the call graph** (`t2-r369`, k cycles a
WRITE of 238.9; `callers-r369.txt`, `under-r369.txt`, `under2-r369.txt`,
`under3-r369.txt`, `schedwhy-r369.txt`):

| | k cycles a WRITE | what is in it | worked before |
|---|---|---|---|
| sleeping, waking, and interrupts hidden at IRQ enable | about 33 | `finish_task_switch` 24.0, 17.1 of it the idle task switching to a woken thread (the hard interrupts that round 61 saw with pseudo-NMI land here), `svc_thread_wait_for_work` 4.2, `nfsd_direct_write` 1.7; `wake_up_process` 5.3 (`svc_pool_wake_idle_thread` 3.9, `svc_pool_hand_idle_thread` 1.4) | rounds 17, 18, 40 (wakes), 47 (NVMe coalescing), 82 (staying, dropped) |
| the loan's receive bookkeeping | about 22 | prefetch 3.3, borrow 3.2, append 3.0, walk 2.7, validate 1.8, span 1.4, region count 1.3 | rounds 77 to 87; 86 and 87 netted 0.75 |
| block submit | 21.7 | `blk_add_rq_to_plug` 11.1 (the SGL map, `blk_rq_dma_map_iter_next` 3.8), `bio_split_rw` 4.0, request alloc 2.3 | rounds 55, 62 |
| **the loan's release** | **14.0** | `rx_loan_put_run` 8.8 (`rx_region_page_put` 2.8, the region's lock released with `irqrestore` 2.0, `rx_region_slot_put` 1.4, `page_pool_recycle_ring_bulk` 1.2, `page_pool_return_netmem` 0.7), the table's own 3.3, `page_pool_put_netmem_bulk` 1.9 | rounds 45, 58 (bulk return), 86 (its prefetch dropped, 0.27) |
| the transmit's qdisc lock | about 7 | `__dev_xmit_skb` 3.1 and its `_raw_spin_lock` 4.1, the ACKs sent by the receive's reads among them | rounds 07, 31, 32, 57: closed |

## The focus suggested for round 89

**The loan's release, 14.0k cycles a WRITE (6% of nfsd's), now the
largest block of the loan's own code**, with the receive's walk worked
down to small gains (rounds 77 to 87). Two thirds of it is
`rx_loan_put_run()` handing pages back to the region and the page_pool,
and 2.0k of that is time with interrupts off under the region's lock,
counted where it ends.  Before any code, a trace in the window: calls to
`rx_loan_put_run()` a WRITE, pages a run, and the region lock's
acquisitions and hold time a WRITE, against the 32 to 40 pages a 1 MiB
WRITE loans at 16 KiB (or as many more as its pieces' pages).  Whatever
is changed reports the region's held peak beside its saving (AGENTS.md:
the footprint is not to grow).

The other candidates, if the release is not taken: the 24k at
`finish_task_switch` cannot be split without a boot with pseudo-NMI
(round 61), which risks a port on tardis2; and block submit's 11k in
`blk_add_rq_to_plug` is the kernel's NVMe path, outside the series.
