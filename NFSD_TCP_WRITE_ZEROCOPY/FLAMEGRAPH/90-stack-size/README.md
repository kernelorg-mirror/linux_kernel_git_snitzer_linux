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

## Result: 128 saves 9k cycles a WRITE, and holds more of the region

`run373.out`, `read65.txt`, `fsum.txt`, `symdiff-64-128.txt`.  Every run
clean: no new kernel warning, no drive stepped down, both ports
answering after each reopen.  The stack is back at 64 in the settled
setting at the end.

| run | stack | | GB/s | nfsd cycles a WRITE | overflow path¹ | the slot lock's irqrestore | loans denied | region figure |
|---|---|---|---|---|---|---|---|---|
| `t2-r373` | 64 | C | 70.83 | 247.1k | 5.10k | 10.93k | 0 | 9,196 (sampled) |
| `t2-r374` | 96 | C | 71.04 | 243.2k | 3.72k | 10.22k | 0 | 9,647 (sampled) |
| `t2-r375` | 128 | C | 71.80 | 240.0k | 2.74k | 9.62k | 19 | 10,954 (sampled) |
| `t2-r376` | 128 | C | 70.55 | 234.5k | 2.59k | 9.57k | 274 | 12,319 (sampled) |
| `t2-r377` | 96 | C | 71.33 | 243.7k | 3.76k | 10.18k | 304 | 11,396 (sampled) |
| `t2-r378` | 64 | C | 70.76 | 245.6k | 5.09k | 10.84k | 0 | 8,637 (sampled) |
| `t2-r379` | 64 | H | 70.25 | | | | 0 | **11,036 exact** |
| `t2-r380` | 96 | H | 72.00 | | | | 0 | **11,210 exact** |
| `t2-r381` | 128 | H | 71.64 | | | | 597 | **12,632 exact** |

¹ `rx_region_page_put` + `rx_region_slot_put` + `page_pool_return_netmem`.

- **The cycles fall with the stack, in both halves of the order.**  Mean
  of each pair: 246.4k at 64, 243.4k at 96 (-2.9k), 237.3k at 128 (-9.1k,
  3.7%).  Of 128's saving, 3.7k is the overflow path and the slot lock's
  irqrestore (`symdiff-64-128.txt`), as round 89 predicted.  The rest is
  spread, `_raw_spin_lock` -0.7k and `napi_pp_put_page` -0.3k the largest.
  The rate is level, 70.6 to 72.0 GB/s.
- **The region holds more at 128.**  The exact held peak is 12,632 pages
  against 11,036 at 64 (+1,596, 14%).  That is more than the 512 that 64
  more entries on 8 pools can hold by themselves, and it is one run each,
  so on its own it is not decisive: the peak has spread from 8k to 12k
  between runs (rounds 77 to 88).  But two other figures point the same
  way.  The sampled figure rises with the stack in both halves (8.9k at 64,
  10.5k at 96, 11.6k at 128), though a larger stack sends fewer pages to
  the slots' caches that it counts.  And **loans were denied at 128 in all
  three runs** (19, 274, 597) and at 96 in one of three (304), against
  none at 64 in any run, in round 88 or round 89.  A denial is the budget
  finding the region short, so the larger stack does hold pages the loans
  then lack.
- 96 is in between: -2.9k cycles a WRITE, +174 exact (within the spread),
  and one denial burst.
- The pools' `ethtool -S` recycle counters saw almost nothing of nfsd's
  returns (0 to 72 pages a run), so the overflow was read from the
  profile instead.

## For Mike

By AGENTS.md's rule ("Do not grow the region's footprint") **128 is not
to be kept**: its saving, 9k cycles a WRITE, comes with a higher held
peak and denied loans.  96 is not shown either way.  Two ways on:

- three more held-mark pairs at 64 and 96, to see whether 96's peak and
  denials are real;
- or keep 64 and take the overflow's cost another way, one that keeps
  the pages in the region's count: a stack that holds no more pages but
  spills less, or the spill made cheaper than the dropped bulk return
  managed.

## Second part: 64 against 96, held mark only (Mike)

Three more pairs with the held mark, `RUNS="64:H 96:H 96:H 64:H 64:H
96:H" N0=382 ./run373.sh` (`t2-r382..r387`, `run382.out`): the exact held
peak and the denied loans at each, to see whether 96's +174 and its one
run with denials are real.

### Result: 96's peak is not shown to differ from 64's

`run382.out`.  All six runs clean (no new kernel warning, no drive
stepped down, both ports answering after each reopen); the stack is back
at 64 in the settled setting at the end.

| pair | 64 | 96 | 96 - 64 |
|---|---|---|---|
| `t2-r379`, `t2-r380` | 11,036 | 11,210 | +174 |
| `t2-r382`, `t2-r383` | 8,019 | 10,082 | +2,063 |
| `t2-r385`, `t2-r384` | 10,462 | 9,803 | -659 |
| `t2-r386`, `t2-r387` | 12,294 (240 denied) | 11,924 (173 denied) | -370 |
| mean | 10,453 | 10,755 | **+302** |

- The exact held peaks at each size spread by 4k pages (8.0k to 12.3k at
  64), and the pairs' differences go both ways (+2,063 to -659).  The
  mean, +302 (3%), is well inside that spread: **96 is not shown to raise
  the peak, nor shown not to.**  Four pairs cannot resolve a difference of
  a few hundred pages against this spread.
- **Denials are not 96's:** `t2-r386`, at 64, denied 240 loans, the first
  at 64 in rounds 88 to 90; `t2-r387` at 96 denied 173 beside it.  So the
  denial burst in `t2-r377` is not evidence against 96 either.  At 128 the
  case stands: denials in all three runs and a peak 1,596 over 64's in the
  same pair of runs.
- The peak drifted upward over the six runs at both sizes (8.0k to 12.3k
  at 64), more than it moved between sizes.

**For Mike:** 96 saves 2.9k nfsd cycles a WRITE (1.2%, counted runs
`t2-r374` and `t2-r377` against `t2-r373` and `t2-r378`) for a held peak
that four pairs cannot tell from 64's.  Keeping it is a change to
`settled.sh` (`RX_REGION_STACK=96`); leaving it at 64 changes nothing.

**Kept (Mike, 10 November): `settled.sh` holds `RX_REGION_STACK=96`.**
Its region figures are watched in the rounds after: a peak that settles
above 64's from here is a reason to go back.
