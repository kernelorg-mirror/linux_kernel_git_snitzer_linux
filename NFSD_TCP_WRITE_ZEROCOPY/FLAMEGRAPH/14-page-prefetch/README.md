# Round 14: prefetch a loaned skb's struct pages for writing

**The change:** "SUNRPC: Prefetch a loaned skb's struct pages for writing" (`02a445664e23`).
- A loaned record examines the `struct page` of every page it takes: `svc_tcp_rx_region_span()` asks if it is a region page, `svc_tcp_rx_borrow_segment()` if it is a page pool's, and the borrow takes a reference. The ring's CPU wrote those lines last, so each first look missed, one page after another.
- Before counting an skb's region pages, `svc_tcp_rx_prefetch_pages()` now walks the skb once and `prefetchw()`s the `struct page` of each page the record's bytes lie in, so the misses overlap and the reference atomics find the line held.
- Only the first step of round 14's plan. Reusing `region_span`'s page-pool answer in the borrow, and one reference a page a skb, are not done.

**Runs 2026-10-24 on tardis2**, build 494, the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run32.sh`.
- A is round 10b's sunrpc (`6D7E259F6D29200D03F15FC`), B round 14's (`FBEBC68D6628C0CE72E3E00`). nfsd is round 13's (`8EF3B4156F6AAF38EC5CD9E`) on both, and both were reloaded before every run.
- KUnit on B: 7, 10, 52 and 7 pass.

## Outcome: kept. nfsd 2% cheaper a WRITE

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE |
|---|---|---|---|
| r14-1-loan64-A | 40.51 | 58.4 | 117.1 |
| r14-2-loan64-B | 40.31 | 59.0 | 114.3 |
| r14-3-loan64-A | 40.53 | 56.7 | 116.1 |
| r14-4-loan64-B | 40.48 | 60.1 | 115.2 |
| r14-5-loan64-A | 40.52 | 59.6 | 118.4 |
| r14-6-loan64-B | 40.51 | 60.0 | 115.1 |

Every run was settled and DRIVE-LIMITED, with no drive stepped down.

- **nfsd:** 117.2 to 114.9 µs a WRITE (-2.3, -2%), with no overlap.
- **The rings:** 58.2 against 59.7. The change runs on nfsd only, and no frame of the rings' profile moved by more than 0.3 µs; read it as the runs' scatter, and watch it in the next rounds.
- **The profile** (fl13, which is A, against fl14), µs a WRITE on nfsd:

| frame | fl13 | fl14 |
|---|---|---|
| `svc_tcp_rx_region_span`, self | 3.22 | 0.98 |
| `svc_tcp_rx_borrow_segment`, self | 4.61 | 3.18 |
| `svc_tcp_rx_prefetch_pages`, self | - | 1.88 |
| `napi_pp_put_page`, self | 2.42 | 2.26 |
| `svc_tcp_recvfrom`, with what it calls | 42.86 | 40.25 |

  - The misses moved into the prefetch walk and overlap there: 3.7 µs out of the two frames for 1.9 in the walk.
  - The borrow still pays 3.2 µs self, and the skb's free (`napi_pp_put_page`) is unchanged: it comes later, once the lines have gone back to the ring's CPU.

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, round 14 | 0.473 ms | 0.545 | 0.594 | 0.472 |
| A1 | 0.461 | 0.545 | 0.602 | 0.473 |
| B2, round 14 | 0.461 | 0.522 | 0.578 | 0.455 |
| A2 | 0.449 | 0.528 | 0.586 | 0.462 |

The same.

**Since the start of the rounds:** a loaned WRITE costs nfsd about 115 µs (184 in round 01's profile) and the rings about 59 (92.6).

## What the borrow still pays

`perf annotate` of fl14: half of the borrow's 3.2 µs self is one load, `compound_head(span_page)` for `page_pool_page_is_pp()`. Its reference atomic (`svc_tcp_rx_get_borrowed`, 0.1 µs) then finds the line held, so that load is what brings the line in.

`xeu-rig/pagetouch.bt` ([pagetouch.txt](pagetouch.txt)) recorded, for 8 s of a loan64 run on round 14, who touched each borrowed page's `struct page` last before the borrow:

| last touch | borrows | how long before |
|---|---|---|
| the ring's frag allocation, on its CPU | 14.3 million (50%) | 256-512 µs |
| this CPU's borrow of the page's other 8 KiB buffer | 12.6 million (44%) | just before |
| another nfsd CPU (skb free or borrow) | 1.4 million (5%) | 8-128 µs |

- So a page's line crosses from the ring once, at its first borrow, as expected; other nfsd threads seldom take it.
- The borrows come 8-16 µs after their skb's prefetch walk. What the load still pays, about 25 ns a page, is the cost of a nearer cache level, not of a remote fetch. A prefetch nearer the borrow, or no load there at all, could win at most about 1.6 µs a WRITE.

**Left as it is:**
- The rest of round 14's plan (reuse the page-pool answer, one reference a page a skb) is worth at most that 1.6 µs, and waits behind larger items.
- tardis2's base for the next rounds is round 14's sunrpc with round 13's nfsd.
