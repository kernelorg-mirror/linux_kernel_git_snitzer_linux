# Round 92: the receive refill path on tardis2

tardis2, build 522 (7.3.3-14.hs.522.canary, 16 KiB), loan mode on two
ports, the settled setting (the pool's stack at 96; the client's
`rpciod` and `xprtiod` strict, round 91).

**Why.**  Round 91 found the server's ring tail drops a steady trickle at
every time scale it could sample, down to 10 ms: in a fifth to a quarter
of 10 ms intervals, a few frames at a time, following neither the frames
arriving nor the ring CPUs' NAPI polls at full budget, and rising with
the rate.  So a ring runs out of posted buffers for a few frames at a
time, under 10 ms.  Mike (10 November): the next area is the refill path.

**The driver, as tardis2 runs it.**  The source is
`kmod-xsight-2.0.1.tar.gz` with kernel-rpmtree's `xsight/` patches 0 to 13
applied as the spec's `%autosetup -p0` does; its module parameters are
exactly the loaded `xeu.ko`'s.  Live: `rx_refill_thresh` 8,
`rx_buf_size` 8192 (two buffers a 16 KiB page), rings of 256 (read from
the ring itself: `twoport.sh`'s `ENTRIES` default).  So a ring has 2 MiB
posted, about 220 us at the 9 GB/s each of a port's four rings takes.

- A ring is refilled only at the end of `xeu_rx_poll()`, once at least
  `rx_refill_thresh` slots are empty (`xeu_rx_refill()`, buffers from
  the ring's page pool, which the receive region backs).
- A refill that leaves the ring short arms a timer to poll again in
  `XEU_RX_REFILL_RETRY_USECS`, 1000 us: about four ring lifetimes.
- A failed allocation prints "Failed to allocate Rx page from pool"
  (rate limited).  There has been none since this boot.
- Reading the ring counters (`ethtool -S`, `ndo_get_stats64`) waits on
  the card's mailbox in `udelay(100)` steps under `spin_lock_bh`, about
  1.4 ms a port: round 91's 10 ms sampling was not free.

**How (`refill.bt`, `run433.sh`).**  No BTF for `xeu`, and the refill is
the compiler's `xeu_rx_refill.isra.0(rxr, n)`, which returns nothing.  The
ring's size (u16 at `rxr+70`), producer index (`+10`) and consumer index
(`+14`) are read where `xeu_rx_poll()`'s code reads them in the loaded
module.  Per ring: at each refill the buffers still posted (size less
`n`), how often fewer than 32 or none were left, and what each refill
posted (the producer index at the next refill less this one's) against
what it asked for.  Per NAPI: the polls, frames, polls at full budget,
the gap before a full-budget poll (the time the card had the ring alone)
and each poll's length.  Then the retry timer, NULL returns of
`page_pool_alloc_pages`, the page pool's slow path and the region's
allocations.  T U T, `t2-r433..r435`: 10 s traced from the start of
twoport's window, against an untraced run, for what the trace costs.
