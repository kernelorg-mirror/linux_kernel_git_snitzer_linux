# Round 27: the receive pass's loan bookkeeping: a look, the prefetch not kept

**What was profiled:** fl51-loan64, round 26 at the settled setting (`t2-r49`, driver `run51.sh`). nfsd cost 102.3 µs a WRITE. It was annotated while that sunrpc was loaded, since a profile recorded through `/proc/kcore` cannot be annotated once its module is unloaded.

## What the annotation found

In fl51 the receive pass that borrows a loaned record's frames costs about 11.8 µs a WRITE inclusive in `svc_tcp_recv_skb`. Its hot instructions are in [annotate.txt](annotate.txt):
- **`svc_tcp_rx_prefetch_pages`** (1.77 µs self): 56% of its samples on the load of `skb_shinfo(skb)->nr_frags`. The skb's shared info was last written by the ring CPU that received it, so the load misses. That is about 0.95 µs a WRITE.
- **`svc_tcp_rx_borrow_segment`** (2.01 self): about 35% on `page_pool_page_is_pp(compound_head(page))`, the frag's `struct page`, about 0.7 µs. The page's prefetch has been issued, but it is still in flight when the walk of the same skb reads the page.
- **`svc_tcp_rx_walk_skb`**: 13% on the frag read.
- **`svc_tcp_recv_skb`**: 13% on `svsk->sk_rx_hold_factor`.
- **`svc_tcp_rx_append_ref`** (2.63): spread out, no single miss.

## The change tried

**"SUNRPC: Prefetch the next skbs' frag lists and pages while a loaned record is read"** (`66bc91c765fa`):
- `svc_tcp_rx_prefetch_ahead()` runs at each skb of a pass:
  - it `prefetchw()`s the shared info of the skb two ahead in the receive queue, its five lines up to `frags[MAX_SKB_FRAGS]`;
  - it prefetches the `struct page`s of the skb one ahead.
- So the frag list and pages of each skb should be in the cache by the time the pass reaches it.

**Runs 2026-10-26 to 27 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run52.sh`, the runs `/var/tmp/zc-run-logs/t2-r50/`.
- A is round 26's sunrpc (`A06FE478658242A10E100FF`), B is the change (`6B753F0B9D05100AC7138C6`, built by `build-sunrpc.sh`, all 327 exports at their versions).
- Before every run sunrpc was swapped and the host settled; no load raised a new WARNING.
- KUnit on B: 7, 52, 7 and 10 pass.

## The A/B: −1.9 µs a WRITE on nfsd

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | segments sent a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|
| r52-1-loan64-A | round 26 | 40.50 | 47.0 | 98.4 | 5.57 | 6539 |
| r52-2-loan64-B | prefetch ahead | 40.51 | 48.0 | 95.1 | 5.54 | 6348 |
| r52-3-loan64-A | round 26 | 40.52 | 48.8 | 100.0 | 5.61 | 6416 |
| r52-4-loan64-B | prefetch ahead | 40.53 | 48.0 | 96.6 | 5.54 | 6510 |
| r52-5-loan64-A | round 26 | 40.55 | 46.3 | 96.6 | 5.59 | 6628 |
| r52-6-loan64-B | prefetch ahead | 40.54 | 47.7 | 97.4 | 5.51 | 6413 |
| r52-7-loan64-A | round 26 | 40.48 | 49.8 | 96.7 | 5.55 | 6474 |
| r52-8-loan64-B | prefetch ahead | 40.53 | 47.8 | 95.0 | 5.52 | 6487 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)). No run stalled: the slowest WRITE of each took 106 to 124 ms.

| per loaned WRITE | A | B | B − A |
|---|---|---|---|
| nfsd, µs | 97.92 ± 1.61 | 96.03 ± 1.17 | −1.90 ± 1.00 |
| rings, µs | 47.97 ± 1.61 | 47.88 ± 0.15 | −0.10 ± 0.81 |
| segments sent | 5.58 | 5.53 | −0.05 |
| enqueues · wakes · switches | 4.79 · 4.00 · 5.47 | 4.74 · 3.99 · 5.46 | about 0 |

- **nfsd:** about two standard errors. Three pairs of four are down: −3.3, −3.4, +0.8 and −1.7.
- **Not changed:** the rate, the region's peak (B 6348 to 6510, inside A's 6416 to 6628), the rings, segments, enqueues, wakes and switches.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## The profile does not show it

fl52-loan64 is B, annotated while loaded ([top-nfsd.txt](top-nfsd.txt), [annotate.txt](annotate.txt)). nfsd busy is 102.29 in fl51 and 101.86 in fl52.

| µs a WRITE on nfsd | fl51, round 26 | fl52, prefetch ahead |
|---|---|---|
| `svc_tcp_rx_prefetch_pages` | 1.77 | 1.46 |
| `svc_tcp_rx_borrow_segment` self | 2.01 | 2.02 |
| `svc_tcp_recv_skb` self, the new code inlined in it | 1.10 | 2.26 |
| `svc_tcp_recv_skb` inclusive | 11.76 | 12.36 |

- **The frag-list miss partly went.** In `svc_tcp_rx_prefetch_pages` the load of `nr_frags` fell from 56% of the samples to 33% of a smaller function, about 0.95 to 0.5 µs.
- **The look-ahead's own loads miss instead.** To find the skbs ahead the change must read `next->next` (20.6% of `svc_tcp_recv_skb`) and the `end` field of the skb after (17.2%). These are `sk_buff` structs the ring CPU wrote, so they miss for the same reason. That is about +1.2 µs self.
- **The `struct page` stall did not move:** about 35% of `svc_tcp_rx_borrow_segment` in both profiles.
- **Net:** the receive pass costs a little more in fl52 (+0.6 µs inclusive). The A/B's −1.9 does not appear where the change runs, so it is recorded as unexplained, not as a gain.

## Why the `struct page` stall does not move

**A diagnostic** (`diagnostic.diff`, `t2-r51`, driver `run53.sh`, sunrpc `767BD8971ED9CBF1FFAD8AF`, one profiled run, fl53, not for keeping): the same change, with the next skb's `struct page` lines *loaded*, not prefetched.
- The load stalls: 21% of `svc_tcp_recv_skb`, so the line is truly a miss one skb ahead.
- Yet `svc_tcp_rx_borrow_segment` still stalls on the same lines: 39% of its samples, against 35%.

**Skbs a pass** ([skbs.bt](skbs.bt), [skbs.txt](skbs.txt), driver `run54skbs.sh`, 10 s of one loan64 run on build 499's own sunrpc, 40.52 GB/s):

| skbs in a `svc_tcp_recvfrom` pass | passes |
|---|---|
| 1 | 41,079 |
| 2 | 1,090,559 (56%) |
| 3 | 363,620 |
| 4 | 334,513 |
| 5 to 7 | 94,330 |

- The skbs are mostly 64 to 128 KiB with 15 frags; none has a frag list.
- About 63% of skbs come after another skb in their pass. In the diagnostic those had their `struct page` lines loaded one skb ahead.
- If that load had worked, the stall in `svc_tcp_rx_borrow_segment` should have fallen by about 60%. It did not fall at all. So the lines are fetched and then lost again within a microsecond or two, before the walk reaches them. A deeper prefetch cannot help that.

**The likely cause, not proven:** each 16 KiB page holds two of xeu's 8 KiB receive buffers. Frames of 16 connections interleave in the ring, so the two halves of a page often belong to different connections, read by different nfsd threads at about the same time. Each thread's work on its half would pull the shared `struct page` line away from the other. Which write does it is not known: the Neoverse N2 here has no SPE to show it.

## Decision

- **Not kept (Mike, 27 October):** the change is off the tip, and the tip before it is `155d8615f3e1`. It is kept on the branch `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.round27-prefetch-ahead`.
- **The deeper prefetch is not warranted:**
  - For the `struct page` stall, distance does not help.
  - For the frag list, at best part of about 0.5 µs is left, since avoiding the look-ahead's own misses needs the `sk_buff` three skbs ahead.
  - 56% of passes have only two skbs, and a pass's first skb can never be fetched ahead.
- **Left:** the `struct page` line shared by the two halves of a page. It is xeu's buffer layout:
  - one buffer a page would double the region's pages per byte, which is ruled out;
  - keeping a page's two halves on one flow is not something the card does.

  A round on it needs a way to see the write first.

## Latency

[latency.txt](latency.txt), untraced runs at iodepth 1:

| | GB/s | p50 | p90 | p99 | mean |
|---|---|---|---|---|---|
| B1, prefetch ahead | 8.37 | 424 µs | 485 | 561 | 433 |
| A1, round 26 | 8.45 | 424 | 473 | 528 | 431 |
| B2, prefetch ahead | 8.42 | 428 | 477 | 537 | 433 |
| A2, round 26 | 8.07 | 444 | 489 | 545 | 452 |

- The same.
