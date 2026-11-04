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
nfsd 270.7k cycles and 272.5k instructions a WRITE counted (rings 72.9k
and 144.6k... as `cost.sh` prints them), no WARNING. The calls were not
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
