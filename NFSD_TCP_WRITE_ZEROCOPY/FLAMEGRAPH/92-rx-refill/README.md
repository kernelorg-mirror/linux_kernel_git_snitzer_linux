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
`rx_buf_size` 8192 (two buffers a 16 KiB page), rings of 128 under load
(`twoport.sh`'s two-port `ENTRIES=128`; 256 at rest, which is what a
first look read).  So a ring has 1 MiB posted, about 110 us at the 9
GB/s each of a port's four rings takes.

- A ring is refilled only at the end of `xeu_rx_poll()`, once at least
  `rx_refill_thresh` slots are empty (`xeu_rx_refill()`, buffers from
  the ring's page pool, which the receive region backs).
- A refill that leaves the ring short arms a timer to poll again in
  `XEU_RX_REFILL_RETRY_USECS`, 1000 us: about nine ring lifetimes.
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

## Result (`t2-r433..r435`, `refread-r433-r435.txt`)

| run | | GB/s | twoport's ring drops (10 s window) |
|---|---|---|---|
| t2-r433 | traced | 67.22 | 10,780 |
| t2-r434 | untraced | 71.84 | 2,100 |
| t2-r435 | traced | 67.11 | 10,862 |

- **The trace is far too heavy:** 4.6 GB/s less and five times the
  drops.  Its probes on every poll (about 290,000 a second, plus the
  tracepoint) cost the ring CPUs that much, so its timings (gaps, poll
  lengths) are of a disturbed system and are not used.
- **Seen from the driver, no ring came near running out at a refill:**
  of 2.42 million refills in 10 s, every one found at least 48 of the
  ring's 128 buffers still posted (most 64 to 128).  The driver's count
  of posted buffers includes those the card has already filled and the
  poll has not reached, so this does not say the card had free buffers.
- **No allocation failed:** no NULL from `page_pool_alloc_pages`, no
  slow path; the region gave 426,000 pages to the pools' stacks.
- **But the retry timer fired about 1,326 times in 10 s**, about 133 a
  second, and only a refill that leaves 8 or more slots empty arms it,
  where the trace's own measure (the producer index at a ring's next
  refill) found no refill short.  One of the two is wrong.

**Next (`short.bt`, `t2-r436`):** a light trace, no probe on the polls:
at `xeu_rx_refill_retry_if_short()`, right after each refill, how many
slots the ring still has empty, and the timer.

## Result (`short.bt`, `t2-r436`)

71.59 GB/s, ring drops 1,788 in twoport's window: the light trace costs
nothing.  In 10 s, 2.68 million refills asked for 99.3 million buffers.

- **No refill comes up short.**  Right after each of 2.58 million
  refills (`xeu_rx_refill_retry_if_short()`), the ring had fewer than 8
  empty slots: every refill posted what it asked for.
- **The retry timer's 1,409 firings are a wrap of the ring's indexes.**
  `xeu_rx_refill_retry_if_short()` is the only caller of `mod_timer` on
  the refill timer, and in the loaded module it computes the empty slots
  as `cons + size - prod` in 32 bits, not masked to the u16 the indexes
  are:

  ```
  f458:	add	w1, w1, w3	// cons + size
  f45c:	sub	w1, w1, w5	// - prod, not masked to u16
  f474:	cmp	w1, w2		// >= rx_refill_thresh: arm the timer
  ```

  So whenever a ring's producer index has wrapped past 65535 and its
  consumer index has not yet, the count is near 65536 and the timer is
  armed though the ring is full.  A ring posts about 1.12 million
  buffers a second (89.5 million in 10 s over eight rings), so it wraps
  every 58 ms: about 17 a second a ring, 137 a second over eight, as the
  timer fires (133 to 141 a second).  `xeu_rx_poll()`'s own count is
  masked (`and w1, w1, #0xffff`), so the refills are right.  The bug is
  in kernel-rpmtree's `xeu-retry-an-RX-refill-that-came-up-short.patch`
  (the C expression `ring->size - (ring->sr_prod_idx -
  ring->sr_cons_idx)` is computed in int); it is harmless (the timer
  only schedules the ring's NAPI, normally running already) and a cast
  to u16 fixes it.

**So the refill path does not explain the drops.**  Allocation never
fails, no refill is short, and at every refill the driver still counts
at least 48 of the ring's 128 buffers posted.  Those include buffers the
card has filled and the poll has not yet reached, so what is left is the
card's side: how many of the posted buffers are still free when frames
arrive, which is a question of how long the card's completions wait for
a poll (a ring of 128 is about 110 us at 9 GB/s).
