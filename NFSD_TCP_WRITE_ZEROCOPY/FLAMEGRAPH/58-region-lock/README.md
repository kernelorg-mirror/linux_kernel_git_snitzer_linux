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
