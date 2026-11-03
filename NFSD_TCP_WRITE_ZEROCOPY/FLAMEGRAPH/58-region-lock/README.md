# Round 58: the region's lock at the release, on two ports

**Planned 3 November after round 57**, which found 2.1 µs a WRITE of nfsd at
the interrupt-enable after the fixed receive region's lock, under
`page_pool_recycle_ring_bulk`, and 0.5 µs in that function. Round 45 measured
the return to the region on one port only. Build 506, the two-port setting
(12 connections a port, 512 KiB, rings of 128, `rx_region_stack` 64).

## The trace

`xeu-rig/regionlock.bt`, 10 s in one loan run on both ports.
- **run168 is not to be read for times** ([regionlock-run168-stretched.txt](regionlock-run168-stretched.txt)).
  Its probe on every page put under the lock ran with interrupts off and
  made each hold about 12 µs, with waits of 4 to 16 µs that the profile does
  not show. Its counts agree with run169's.
- **run169** ([regionlock.txt](regionlock.txt), [run169.sh](run169.sh),
  66.01 GB/s, about 590,600 WRITEs) has no probe under the lock. It counts
  the holds from outside and the waits from the kernel's lock-contention
  events.

| a WRITE | |
|---|---|
| calls of `page_pool_recycle_ring_bulk()` | 4.87, mostly of 14 to 17 pages |
| of them overflowing into the region, one hold of its lock each | **1.78** |
| pages back to the region | **19.4** (10.9 a hold; 16 in 29% of holds) |
| time a hold, entry to exit of `page_pool_return_netmem_bulk()` | 2.1 µs (1 to 4 µs), so about 3.8 µs a WRITE |
| holds that waited for the lock | 0.033 (1.9% of holds), mostly 1 to 2 µs |
| the rings' refills from the region | 0.96, about 20 pages each |

- **The lock is not contended.** The cost is the hold itself.
- **The hold is misses:** in run170's PMU recording (round 55's), the
  interrupts-off section that the hold ends in (`_raw_spin_unlock_irqrestore`)
  runs at IPC 0.50 with 29.6 L2 refills a thousand instructions, against
  nfsd's 1.21 and 14.6.
- **A third of a WRITE's pages go round:** 19.4 of the 64 pages a 1 MiB WRITE
  holds go back to the region and are taken again by the rings, because the
  pools' stacks of 64 overflow. A deeper stack was turned down for the
  footprint (rounds 04 and 19), and round 45 made the trip one hold a bulk.

What the hold touches, with interrupts off: the lock's line, the device's
free-list count and the list's top slots, which the rings read and every
releasing CPU writes; each page's slot byte in `owner[]`; and after the
unlock, the pool's release count, once a page.

## The changes (code group, at the tip)

- `page_pool: count a bulk returned to the receive region with one atomic`:
  one `atomic_add()` for the bulk on the pool's release count, which every
  releasing CPU writes, instead of one `atomic_inc_return_relaxed()` a page.
  The count for each page is kept while the `page_pool_state_release`
  tracepoint is on.
- `page_pool: prefetch the region list's top before taking the region's lock`:
  `prefetchw()` the device list's count and the first and last slots the
  bulk will fill, before the lock, so that those misses overlap the lock's.
  The top is read without the lock, as a hint only.

**Estimate: 0.5 to 1.5 µs a WRITE, a guess.** Neither change holds a page
longer or keeps more of them: the footprint cannot rise.

**To test, on the kernel built from the tip:** `page_pool.rx_region_bulk_ab`
N returns a bulk as before, Y (the default) with both changes. From this
directory: `../ab-switch.sh pp t2-r171 > run171.out`. The region's mean in use
and peak are reported beside each run, and `regionlock.bt` again on B gives
the hold's new time.

## The A/B on build 507 (tardis2, 3 November), the loan runs

`../ab-switch.sh pp t2-r171` (`run171.out`, logs in `$L/t2-r171`), both
ports, 12 connections a port at 512 KiB, rings of 128, budget 128;
`blk_mq_dma.map_iter_inline` at Y throughout. sunrpc
`2A512644C685341146A3A9B`, nfsd `BB6D1949646AB051A8D99CC`, xeu
`CA0931345E46C929B11D01F`.

| run | GB/s | nfsd cycles a WRITE | rings | region in use at the middle | peak |
|---|---|---|---|---|---|
| pp-1-A (N) | 66.05 | 268.2k | 144.2k | 2815 (44 MiB) | 3540 |
| pp-2-B (Y) | 66.64 | 271.3k | 144.1k | 3107 (49 MiB) | 3703 |
| pp-3-B (Y) | 66.96 | 270.8k | 144.2k | 3203 (50 MiB) | 3489 |
| pp-4-A (N) | 65.94 | 270.0k | 144.5k | 2807 (44 MiB) | 6458 |

A, build 506's return, reproduces round 57's baseline (273.1k nfsd,
144.6k rings). With both changes on, nfsd takes 271.1k cycles a WRITE
against 269.1k (+0.7%), rings the same: the 0.5 to 1.5 µs saved is not
there. The rate is 1.2% higher on B in both pairs (66.80 against 66.00
GB/s), and the region holds about 350 pages more at the middle, which
the changes cannot cause by holding (they hold nothing longer) and may
follow the rate. The peaks are mixed (A's 6458 is one spike). No
alloc_fail, no stall, no step on any run. The switch turns both changes
together, so this does not say which of them, if either, costs the 2k.
`lat.sh`, the profile of B and `regionlock.bt` follow.

The rest of `run171.out`: `lat.sh` at iodepth 1, A B B A then B A A B,
A 7.76, 7.82, 7.70, 8.42 GB/s (mean 7.93, mean completion 0.457 ms), B
8.18, 7.85, 8.33, 8.42 (8.20, 0.445 ms): B ahead by about 3% in both
sets, within reach of the spread (l7-A and l8-B tie). The profile of B
(`fl-pp`, 69.39 GB/s; rings busy 31.54 µs a WRITE, nfsd 90.77):
`_raw_spin_unlock_irqrestore` 3.65% of nfsd's cycles and 4.91% of the
rings', `page_pool_put_netmem_bulk` 2.50% of nfsd's inclusive, against
3.93%, 4.92% and 2.68% in round 56's profile on build 506
(`t2-r165/fl56`, the same setting, another boot, rings 34.39 µs and
nfsd 98.52). By that, about 0.5 µs less of nfsd's time a WRITE in the
interrupts-off section, but across two boots whose busy times differ by
8%. So `run173.sh` traces the lock's hold on this boot both ways, B
then A.

**The lock's hold on this boot** (`run173.sh`, `run173.out`;
[regionlock-507-B.txt](regionlock-507-B.txt),
[regionlock-507-A.txt](regionlock-507-A.txt); one loan run each, the
trace for 10 s, as run169):

| | a hold | a WRITE | holds a WRITE | pages back a WRITE | holds that waited | GB/s |
|---|---|---|---|---|---|---|
| B (Y) | 2.00 µs | **3.61 µs** | 1.80 | 19.9 | 1.8% | 66.87 |
| A (N) | 2.30 µs | **4.10 µs** | 1.79 | 19.7 | 2.8% | 69.31 |
| run169, build 506 | 2.14 µs | 3.80 µs | 1.78 | 19.4 | 1.9% | 66.01 |

The changes take 13% off the hold, 0.5 µs a WRITE, the low end of the
estimate, with the same number of holds and pages. A ran 3.6% faster and
waited more, which flatters B a little. Yet the four loan runs above put
nfsd's cycles a WRITE 2k higher with them on, not lower: the time saved
under the lock does not show in nfsd's total. One trace each.

**Kept (Mike, 3 November).** Both changes stay in the code group; only
the A/B switch goes, with the DROP commit at the next regroup.
