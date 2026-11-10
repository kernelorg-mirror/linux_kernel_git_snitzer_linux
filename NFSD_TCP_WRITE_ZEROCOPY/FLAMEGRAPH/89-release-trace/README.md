# Round 89: the loan's release, traced

10 November, tardis2, build 522's second boot, the tip's sunrpc from
`updates/` (as round 88).

Round 88 found the release the largest block of the loan's own code:
14.0k cycles a WRITE in `rx_loan_table_release()`, 8.8k of it under
`rx_loan_put_run()` (`rx_region_page_put` 2.8k, the irqrestore of a lock
2.0k, `rx_region_slot_put` 1.4k, `page_pool_recycle_ring_bulk` 1.2k,
`page_pool_return_netmem` 0.7k).  In the code, a released page goes back
in batches to its pool's stack (`page_pool_recycle_ring_bulk()`, the
producer lock once a batch), and what does not fit the stack
(`rx_region_stack` 64) leaves the pool one page at a time through
`page_pool_return_netmem()` into the region: `rx_region_page_put()`,
then the slot's per-CPU cache in `rx_region_slot_put()`, a lock taken with
interrupts off for each page.  So the region's per-page costs are the
stack's overflow.  This round counts it before any code.

`rel.bt`, for 10 s inside the window of each of two two-port loan runs
(`run371.sh`, `t2-r371`, `t2-r372`): releases and their entries, runs of
entries on one page, the bulk returns and their pages, the stack's depth
at each and its overflow, the pages leaving the pool one at a time and
which way they go into the region, the time of a slot put, and the way
back (stack refills, region allocs, slot takes, DMA maps).  No probe runs
under a lock (round 58).

## Result: a quarter of the pages that reach the stack overflow it

`run371.out`, `relread.txt` (by `relread.py`), `rel-r371.txt`,
`rel-r372.txt`.  Both runs clean, no new kernel warning: 71.56 and 70.13
GB/s with the trace on for 10 s of the 40.  The two runs agree to within
2% on every count.

| a WRITE | `t2-r371` | `t2-r372` |
|---|---|---|
| table entries released | 132.2 | 132.4 |
| pages put (`rx_loan_put_run()`), 82% of them two entries | 70.2 | 70.1 |
| pages into `page_pool_put_netmem_bulk()`, in 4.6 calls | 67.2 | 67.4 |
| of them the last reference, to the pool's stack (`page_pool_recycle_ring_bulk()`, 4.4 calls) | 59.0 | 59.3 |
| **of them over the stack's 64, back one at a time** (1.4 calls of the 4.4) | **14.9** | **15.0** |
| `page_pool_return_netmem()` from nfsd | 14.1 | 14.2 |
| `rx_region_slot_put()`: this CPU's cache of the slot, a lock with interrupts off each | 12.2 | 12.4 |
| to the shards (`rx_region_page_put()` less the slot puts) | 1.2 | 1.2 |
| rings: pages taken back from the slots' caches and lists, in 0.7 region allocs | 10.5 | 10.7 |
| rings: stack refills | 3.05 | 3.09 |
| `napi_pp_put_page()` on nfsd's CPUs | 9.8 | 10.0 |

- **A 1 MiB WRITE returns 70 pages** (132 entries, two 7.8 KB buffers to
  a 16 KiB page), 59 of them as the last reference to their pool.  The
  other 11 are still held by another record or skb and return there.
- **15 of the 59 (25%) find the stack full** and leave the pool one at a
  time: `page_pool_return_netmem()`, `rx_region_page_put()`, and the
  slot's per-CPU cache under its own irqsave lock.  This is round 88's
  profile: `rx_region_page_put` 2.8k, the irqrestore 2.0k,
  `rx_region_slot_put` 1.4k, `page_pool_return_netmem` 0.7k, about **7k
  cycles a WRITE, about 470 a page over**.  The rings take 10.5 pages a
  WRITE back from the slots in region allocs, so most of the overflow
  goes round and comes back to the pool it left.
- **The stack's depth at a bulk return is spread from 0 to 64**, and 19%
  of the calls find it full.  So the stack is not short on average.  It
  takes a WRITE's pages in bursts: 59 pages in 4.4 calls a WRITE, from 48
  threads, on 8 pools.
- Nothing escaped the region (`escaped 0` in
  `/sys/kernel/debug/page_pool_rx_region` after both runs).  The 1.2 a
  WRITE to the shards are pages mapped for no device:
  `page_pool_rx_region_put()` is inlined there, so its probe counted 0.
- The traced times are not the code's: `rx_loan_table_release()` reads
  48 us a call with about 120 probes under it, against about 7 us in round
  88's profile, and a slot put 0.4 us with its own probe pair.  The counts
  stand, the times do not.

The bulk return of the overflow to the slot under one lock was tried on
7 November and dropped: 3k to 5.5k fewer instructions a WRITE but no
fewer cycles (`HANDOFF-tardis2-canary.md`, "The second boot (7 November,
after the power reset): dropped").  So the lever is less overflow, not
cheaper overflow.

## Suggested next: the stack's size, a setting (round 90)

`page_pool.rx_region_stack` is a setting, applied with `settled.sh`'s
`EXP_RX_REGION_STACK` (the ports reopened), and needs no code.  A larger
stack keeps the burst's pages in their pool, where the ring takes them
without the region's round trip.  But a page on a stack is held, and a
page in a slot's cache is free.  So at most 64 more pages a pool, 512 for
8 pools (about 4% of the 11.9k peak), may be held at the peak, and the
footprint is not to grow (AGENTS.md, round 04's 1024: 940 more pages).
The A/B to decide it, at 64 against 96 and 128: the overflow (`rel.bt`),
nfsd's cycles a WRITE and the region's exact held peak (held mark) for
each, in turns.  Whether to try it is Mike's call.
