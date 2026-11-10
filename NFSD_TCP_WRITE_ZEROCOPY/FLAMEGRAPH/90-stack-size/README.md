# Round 90: the pool's stack, 64 against 96 and 128

10 November, tardis2, build 522's second boot, the tip's sunrpc from
`updates/` (as rounds 88 and 89).

Round 89 found 15 of the 59 pages a WRITE returns to its pool's stack
overflow `page_pool.rx_region_stack`'s 64 and go back to the region one at
a time, about 7k cycles a WRITE.  A larger stack keeps them, but a page on
a stack is held where one in a slot's cache is free: at most 64 more pages
a pool (512 for 8 pools at 128) at the peak.  The footprint is not to grow
(AGENTS.md), so the held peak decides as much as the cycles.

`run373.sh`: two-port loan runs as round 88's, the stack set through
`settled.sh`'s `EXP_RX_REGION_STACK` (the ports opened again, then pinged):

- `t2-r373..r378`: counted profile, the held mark off, stacks 64 96 128
  128 96 64;
- `t2-r379..r381`: the held mark on, no profile, stacks 64 96 128, for
  the region's exact held peak.

Each run keeps both ports' `ethtool -S` before and after: the pools'
`recycle_ring` (pages to the stack), `recycle_ring_full` (bulk returns
that overflowed it) and `alloc_slow` (allocs from the region).  The stack
is put back to 64 at the end.
