# Round 60: sunrpc's own loan frames, at the two-port setting

**The plan (Mike, 4 November), from round 55's B profile on build 507**
(`t2-r172/fl-blk`, 67.31 GB/s, nfsd busy 88.79 µs a WRITE): the frames
that are sunrpc's alone, about 6.6% of nfsd's samples, very roughly 8 µs a
WRITE: `svc_tcp_rx_append_ref` 1.71%, `svc_tcp_rx_borrow_segment` 1.45%,
`svc_tcp_rx_release_refs` 1.12%, `svc_tcp_rx_walk_skb` 0.88%,
`svc_tcp_rx_prefetch_pages` 0.86%, `svc_tcp_rx_region_span` 0.57%. The
other targets of the B profile are closed (wakes, round 40; ACKs, round
57) or bounded by the footprint (the region's lock, round 58). A change is
built as a sunrpc module and put in `updates/` (Mike: allowed for this
round).

**What round 36 found in the same frames** (build 499, one port): they are
serial chains of near loads, L1 hits (round 36's r77), not far misses;
about 131 borrows and 140 appends a WRITE, one a received frame; and on a
rig its drives limit, cycles saved show as idle, not rate. Kept then:
`eae183aaaff3`, the borrow's page-pool test from one load and the append's
cheap conditions first. Rounds 38 and 39 worked the release's and the
receive side's struct page misses.

**Step 1, the look** ([run174.sh](run174.sh), `$L/t2-r174`): one loan run
on both ports, counted, with samples of cycles, backend memory stalls and
L1D refills on nfsd's CPUs, annotated, and the frames' calls a WRITE. Then
a change judged by instructions and cycles a WRITE, counted, A/B on one
boot.

## 1. The look (r174)

[run174.sh](run174.sh), `$L/t2-r174`: 66.84 GB/s, no drive stepped down,
nfsd 270.7k cycles and 272.5k instructions a WRITE counted, no WARNING. The calls were not
counted: the probe list named `svc_tcp_rx_skb_region_pages_after`,
which is inlined, and bpftrace attached nothing (run175 counts them).

Each record window ran longer than its 4 s of samples, so the samples are
put over 4 s at the run's 64,030 WRITEs a second
([samples-cycles.txt](samples-cycles.txt),
[samples-stall_backend_mem.txt](samples-stall_backend_mem.txt),
[stat-nfsd.txt](stat-nfsd.txt)). Cycles a WRITE on nfsd:

| frame | cycles | backend memory stall |
|---|---|---|
| `svc_tcp_rx_append_ref` | 4.7k | 1.0k |
| `svc_tcp_rx_borrow_segment` | 3.9k | 0.7k |
| `svc_tcp_rx_release_refs` | 3.0k | 0.8k |
| `svc_tcp_rx_walk_skb` | 2.4k | 0.4k |
| `svc_tcp_rx_prefetch_pages` | 2.3k | **1.8k** |
| `svc_tcp_recv_skb` | 2.3k | 0.8k |
| `svc_tcp_rx_region_span` and `..._pages_after.part.0` | 2.7k | **1.3k** |
| `svc_tcp_rx_validate_mixed` | 1.1k | 0.6k |

About 22k cycles, 8.3% of nfsd's. Two of them are mostly stall, and both
are first touches
([prefetch](annotate-stall_backend_mem-svc_tcp_rx_prefetch_pages.txt),
[region span](annotate-stall_backend_mem-svc_tcp_rx_region_span.txt)):

- **The prefetch walk is the first reader of each skb's shared info.** 34%
  of its stall samples follow the load of `skb_shinfo(skb)->nr_frags`, and
  about 21% follow the loads of each `frags[]` entry: lines the ring's CPU
  wrote last. Its `prefetchw()`s cost nothing.
- **The region count reads each struct page just after its prefetch was
  issued.** 28% of its stall samples follow `page->pp_magic` and 30% the
  `pool->rx_region` load that waits on `page->pp`, in the same struct page
  line. The pool's line at 960 holds only read-mostly fields
  (`user.id`, `rx_region`, `rx_region_dev`).

## 2. The skbs' shape and the calls (r175)

[run175.sh](run175.sh), `$L/t2-r175`, 68.67 GB/s
([calls-r175.txt](calls-r175.txt), [shape-r175.txt](shape-r175.txt); a
WRITE is the run's 66,556 a second over the 4 s counted):

- **About ten skbs a WRITE,** most of 64 to 128 KiB, with 7 or 14 to 15
  frags (GRO's page fragments), **none with a frag list.**
- **Calls a WRITE:** 159 borrows, 172 appends, 111 region spans, 75 put
  runs, about 10 prefetch walks and 11 skb walks.

So the prefetch walk's stall is about ten skbs a WRITE, each the first
reader of some five lines of shared info, and nfsd takes some 2k cycles
over each skb: room enough to fetch the next one's lines while it does.

## 3. The change

`af250a0051d5` "SUNRPC: Prefetch the next skbs of a loaned record": while
one skb of a loaned record is taken, the struct pages of the next skb on
the receive queue are prefetched, and the shared info of the one after it
(every line of it, frags included, without reading its frag count),
whose struct pages are then fetched at the next skb. So the region count
and the borrow find both in place. `->read_sock` runs under the socket
lock, so the queue holds still (segments that arrive go to the backlog);
only TCP's own `->read_sock` hands over skbs on it, and the lookahead is
limited to that. Nothing is held: no page, no skb.

`399de17d35f5` "DROP: SUNRPC: A/B switch for FLAMEGRAPH round 60":
`sunrpc.svc_tcp_rx_lookahead`, 0 as before, 1 the next skb's shared info
only, 2 (the default) the change. Built with `build-sunrpc.sh` (sunrpc
`796267B1BA39136191F34A9`, exports unchanged) and put in `updates/` by
`swap-sunrpc.sh` (Mike: allowed for this round), then the settled setting.

**To test:** `../ab-switch.sh look t2-r176 > run176.out` from here: A 0,
B 2, as rounds 55 and 58.

## 4. The A/B on build 507 (r176), the loan runs

`../ab-switch.sh look t2-r176` (`run176.out`), both ports, 12 connections
a port at 512 KiB, rings of 128, budget 128; rounds 55's and 58's changes
on.

| run | GB/s | nfsd cycles a WRITE | instructions | region in use at the middle | peak |
|---|---|---|---|---|---|
| look-1-A (0) | 66.86 | 271.9k | 273.1k | 3083 | 3583 |
| look-2-B (2) | 67.16 | 270.5k | 274.8k | 3095 | 3531 |
| look-3-B (2) | 66.89 | 270.2k | 275.5k | 3326 | 3416 |
| look-4-A (0) | 66.53 | 270.1k | 272.6k | 2714 | 3549 |

Cycles 271.0k to 270.4k a WRITE (-0.2%), inside the spread and far from
the 3k of stall aimed at (look-4-A alone matches the B runs);
instructions +2.3k, the walk and the prefetches added. Rate level (66.70
against 67.03 GB/s); the region's middle 2900 to 3210 pages, the peaks
level, and the change holds nothing.

The rest of `run176.out`: `lat.sh` at iodepth 1, A 8.16, 7.97, 8.30, 8.17
GB/s (mean 8.15, mean completion 0.444 ms), B 7.92, 8.18, 7.49, 7.92
(7.88, 0.456 ms): B slower in three pairs of four. The profile of B
(`fl-look`, 68.86 GB/s, nfsd busy 92.40 µs a WRITE) against round 55's B
(`fl-blk`, 88.79): `svc_tcp_rx_region_span` 0.57% to 0.41% of nfsd's
samples, but `svc_tcp_recv_skb` 0.81% to 1.19% and
`svc_tcp_rx_prefetch_pages` 0.86% to 1.05%.

## 5. Where the stalls went (r177)

[run177.sh](run177.sh), `$L/t2-r177`: one loan run at 65.78 GB/s, the
switch at 0, 2, 0 in turn, 4 s each of backend memory stall samples and
of cycle samples on nfsd's CPUs, each window's WRITEs counted from perf's
own start.

| cycles a WRITE | 0 (two windows) | 2 |
|---|---|---|
| `svc_tcp_rx_region_span` | 1.51k | **1.11k** |
| `svc_tcp_rx_prefetch_pages` | 2.42k | 2.86k |
| `svc_tcp_recv_skb` (the lookahead is inlined there) | 2.12k | **3.13k** |
| stall in those two | 2.20k | 3.20k |
| sunrpc's loan frames in all | 22.4k | **23.5k** |

- **The struct pages arrive:** the region count is 0.4k cheaper.
- **The lookahead makes first touches of its own**
  ([recv_skb at 2](annotate-stall-recv_skb-r177-2.txt)): 25% of
  `svc_tcp_recv_skb`'s stall samples follow `next->len`, a line of the
  next skb's `sk_buff` nothing has read yet.
- **The shared info prefetched a step ahead does not arrive**
  ([the prefetch walk at 2](annotate-stall-prefetch_pages-r177-2.txt)): 43%
  of the walk's stall samples still follow `nr_frags`. The plain
  `prefetch()` of five lines is likely dropped, as round 38 found the
  release's were.

So the change costs 1.2k cycles a WRITE in sunrpc's frames, the loan runs
put the whole at -0.2% (inside the spread) and latency at iodepth 1 is no
better. The switch is at 0 until it is decided (Mike); `updates/` still
holds the A/B's sunrpc (`796267B1BA39136191F34A9`).

**What another form would need:** every load of the lookahead on a line
prefetched a step before: the `sk_buff` lines (`next`, `len`,
`head`/`end`) three skbs ahead, the shared info two ahead with a prefetch
that is kept (round 38's kinds), the struct pages one ahead. At best that
recovers the 3k cycles of stall measured, less its own instructions:
perhaps 2k a WRITE, under 1% of nfsd's.
