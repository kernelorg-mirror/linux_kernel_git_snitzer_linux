# Round 77: the loan's per-segment bookkeeping, measured

Round 76 found the loan's bookkeeping the largest block of the series'
own code on the canary (about 28k cycles a WRITE) and asked, before any
code, for a profile with call graphs and the region's exact peak.  Both
ran on 9 November on the canary's second boot, its own stack, the
settled setting ([run326.sh](run326.sh), [run326.out](run326.out)):

| run | | GB/s | WRITEs | retransmits | region |
|---|---|---|---|---|---|
| `t2-r326` | held mark on, round 74's counted profile | 70.08 | 66744 | 1051 | `max_held` **8092** exact (sample beside it: median 3276, p99 6182, at most 7385) |
| `t2-r327` | mark off, cycles with call graphs (`--call-graph fp`) | 69.61 | 66246 | 2783 | sample at most 9551 |

No new kernel warning, no drive stepped down, no stall.

## The region: no rise

514's exact peak was 8949 and 9134 pages (`t2-r199`, `t2-r200`, the
held-mark A/B in `HANDOFF-tardis2-canary.md`); the canary's is 8092.
Round 76's README set the sample beside "about 7,600" for 514, which was
the prose of `xeu-rig/README.md` and not a measured exact peak: the
figure to set beside is 8949 to 9134, and the canary is under it.  One
run, so not "lower" either: the same.

## Where the bookkeeping runs ([callers.py](callers.py), [under.py](under.py), [callers-r327.txt](callers-r327.txt), [under-r327.txt](under-r327.txt))

`t2-r327`, 231.4k cycles a WRITE on nfsd's CPUs, the idle loop out,
inlined frames folded into the function that holds them.  A 1 MiB WRITE
at MTU 7792 is about 135 frames, so a frag costs about 1 cycle a WRITE
per 135.

**Receive, under `svc_tcp_recv_skb()`: 22.2k**, called once an skb from
`tcp_read_sock()`.  Three passes over the same frags, in turn:

| pass | incl | what |
|---|---|---|
| `rx_loan_prefetch_pages()` | 2.9k | `prefetchw` of each frag's `struct page` |
| `rx_loan_skb_region_pages()` > `rx_loan_region_span()` | 2.7k | counts the region pages, for the credit admitted before any borrow; reads the page's pool (`page_pool_page_in_rx_region()`) |
| `svc_tcp_rx_walk_skb()` > `svc_tcp_rx_borrow_segment()` > `__rx_loan_table_append_ref()`, `svc_tcp_rx_append_ref()` | 10.7k | the walk that borrows: 2.6k its own, borrow 3.3k, append 4.3k and 1.1k; it reads the page's pool again (`svc_tcp_rx_page_is_pp()`) |
| the rest | 5.9k | `svc_tcp_recv_skb()` itself 2.0k, credit and admission 1.4k, the copied prefix 0.8k |

Then at publish `svc_tcp_rx_validate_mixed()` walks the table once more,
1.6k, and `svc_tcp_restore_pages()` 0.8k.

**Release, under `svc_tcp_rx_release_refs()` from
`svc_tcp_release_ctxt()`: 13.8k**, nearly all in
`rx_loan_table_release()`: 3.1k its own, then the pages going back,
`rx_region_page_put()` 2.8k, `_raw_spin_unlock_irqrestore` 1.9k (the
region's lock, irqs masked, so partly time spent under it),
`page_pool_put_netmem_bulk()` 1.4k, `rx_region_slot_put()` 1.3k,
`page_pool_recycle_ring_bulk()` 1.1k, `page_pool_return_netmem()` 0.7k.

So the loan's own code is about 22k at receive and 14k at release, 16%
of nfsd's cycles: more than round 76's 28k by name, since the call graph
adds the page_pool and region functions release calls.

**Other callers seen on the way** (not the loan's): half of
`_raw_spin_lock` (4.0k of 8.1k) is `__dev_xmit_skb()`, the qdisc's lock
on the reply's send (rounds 07 and 31 have the qdisc); 1.5k is
`svc_recv()`.  Half of `_raw_spin_unlock_irqrestore` (5.0k) is
`wake_up_process()`.  The slow path is `__lwq_dequeue()` 0.7k and
`lock_sock_nested()` 0.4k.  `blk_rq_dma_map_iter_next()`, 4.9k, is 94%
from `nvme_pci_setup_data_sgl()`.

## Suggested focus

1. **One pass for the prefetch and the count** (5.6k together).  They
   walk the same frags back to back, with the same cursor arithmetic and
   the same recursion into the frag list; the count can be taken in the
   prefetch's loop, each page counted when it is fetched.  The admission
   still comes before any borrow, so the order that keeps loans safe is
   untouched, and no page is held longer.  What it can save is the
   second loop and its pool lookup, not the misses: those are what the
   prefetch is there to take, and the pass that takes them keeps them.
2. **Then the per-entry cost of append and release** (4.3k and 3.1k,
   about 30 and 23 cycles an entry at high IPC): annotate
   `__rx_loan_table_append_ref()` and `rx_loan_table_release()` from a
   counted profile (`perf annotate` needs the `perf.data`, so keep one)
   before anything is changed, to see whether it is the table's own
   bookkeeping or the reference counts.
3. The page returns of release (about 9k in page_pool and the region)
   are where rounds 55 and 58 worked; anything there has to keep pages
   returned last in, first out and report `max_held` beside it.
