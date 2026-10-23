# Round 03: `net.core.skb_defer_max` 0, so a consumed skb is freed by nfsd

**The change:** `net.core.skb_defer_max` goes from 128 to 0.
- With 128, an skb nfsd has consumed is handed back to the CPU that received it and freed there, in the ring's softirq (`skb_defer_free_flush`).
- With 0, the nfsd thread that consumed it frees it at once.
- It is a sysctl, so the test needed no reload. `twoport.sh` took `SKB_DEFER_MAX=` for it, and `settled.sh` held each run to the value it asked for.

**Run 2026-10-23 on tardis2**, build 494, sunrpc with round 02 (`1CC95EF56506263BC30D23B`), the settled setting with round 01. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run20.sh`.

## Why: round 02's ring ranking

`skb_defer_free_flush` cost the rings 8.85 µs a WRITE. Of that, `napi_pp_put_page` was 7.01 µs of self time.

That put drops the skb's own reference on a page the loan also holds, so it is never the last one. It costs what it does because the page's `struct page` line was last written by the nfsd CPU that took the loan's reference. Freed by that same nfsd thread, the line is local.

128 was set on 9 October for copy mode, which gave the same rate either way (27.3 and 28.0 GB/s). It had never been measured as ring time per loaned WRITE.

## Outcome: kept. The rings 15% cheaper a loaned WRITE, and nfsd cheaper too

| run | `skb_defer_max` | GB/s | ring softirq | ring µs a WRITE | nfsd µs a WRITE | refusals |
|---|---|---|---|---|---|---|
| r3-1-loan64-A | 128 | 40.25 | 77.3% | 80.6 | 169.7 | 0.62 |
| r3-2-loan64-B | 0 | 40.42 | 66.7% | 69.1 | 161.6 | 0.92 |
| r3-3-loan64-A | 128 | 40.42 | 75.0% | 77.7 | 165.3 | 0.95 |
| r3-4-loan64-B | 0 | 40.47 | 63.4% | 65.8 | 156.5 | 1.02 |
| r3-5-loan64-A | 128 | 40.41 | 78.6% | 81.6 | 167.6 | 0.84 |
| r3-6-loan64-B | 0 | 40.42 | 66.5% | 69.2 | 161.4 | 0.98 |

Every run was untraced, settled to the value it asked for, and DRIVE-LIMITED, with no drive stepped down.

- **The rings:** 80.0 to 68.0 µs a WRITE, -12.0 µs (-15%), with no overlap between the two sets.
- **nfsd:** 167.5 to 159.8 µs a WRITE, -7.7 µs. It took on the frees and still came out cheaper.
- **No fit needed:** every B run had as many refusals as the most of the A runs, or more (0.92-1.02 against 0.62-0.95). Refusals raise the rings' cost, so the gain is at least what the table shows. Rings that empty sooner refused more in round 01 as well.

**The profile with 0** (fl3-loan64, [rings.svg](rings.svg), [nfsd.svg](nfsd.svg)): the rings 70.4 µs a WRITE, nfsd 165.7.
- `skb_defer_free_flush` and `napi_pp_put_page` are gone from the rings.
- On nfsd, `napi_pp_put_page` costs 2.68 µs a WRITE: the 7.0 it cost the rings, now on a line the CPU already holds.
- `skb_attempt_defer_free` costs nfsd 3.45 µs a WRITE in all.

**The setting:**
- `xeu-rig/settled.sh` holds `net.core.skb_defer_max=0`, and so does `/etc/sysctl.d/90-zc-settled.conf` on tardis2.
- tardis1 takes it at its next `./settled.sh apply`. If tuned's `hs-e1` writes 128 again, the audit says BAD.
- Copy mode was not measured (loan mode is the target).

**Since the start of the rounds:** a loaned WRITE costs the rings 68.0 µs, against 92.6 before round 01 (-27%).

## For round 04: the region pools' page stack overflows on 7% of pages

The page_pool counters were read around each run. They start again when the port opens, so the reading after a run is that run's own total:

| per loaned WRITE (64 pages of 16 KiB) | each run, A or B |
|---|---|
| pages returned to the pool | 63.0-63.4 |
| `recycle_ring_full`, counted once a batch that overflows | 0.64-0.70 |
| `alloc_slow`, refills from the region | 0.27-0.30 |
| `recycle_cached` | about 0 |

The region-bound pools recycle through `rx_stack`, our LIFO, not the ptr_ring. `ppsize.bt` read its size, the pool's `ring.size`: 256 pages a pool, one pool a ring.
- In the profiled run `page_pool_return_netmem` ran 9.70 million times, over 2.14 million WRITEs. That is **4.5 of each WRITE's 64 pages (7%) given back to the region**: the DMA unmapped, then mapped again when the ring takes them back.
- With 0, that costs nfsd 3.89 µs a WRITE (`page_pool_return_netmem`, mostly the unmap) and the rings 1.19 (`page_pool_alloc_netmems`, of it 0.77 in `page_pool_rx_region_alloc`).
- That is about 5 µs a WRITE, in our own code. It is enough for the knob (Mike: "add the knob if the measurement justifies it"): a size for region-bound pools' stack. It takes a kernel build and a boot.
