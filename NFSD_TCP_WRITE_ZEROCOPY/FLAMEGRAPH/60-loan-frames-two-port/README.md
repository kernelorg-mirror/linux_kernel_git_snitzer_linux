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
