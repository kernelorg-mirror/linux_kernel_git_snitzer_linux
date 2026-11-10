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
