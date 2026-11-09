# Round 78: the region count taken in the prefetch's walk

**The change (Mike, 9 November, from round 77):** `svc_tcp_recv_skb()`
walks each loaned skb's body twice before borrowing from it,
`rx_loan_prefetch_pages()` (2.9k cycles a WRITE) and then
`rx_loan_skb_region_pages()` (2.7k).  **B** counts the region pages in
the prefetching walk, `svc_tcp_rx_prefetch_count()` in `svcsock.c`
(`kernel-7.3/canary.PC`, `f9eb67570da8`, worktree `/dev/shm/wt-PC`).
The count reads the struct page it is given, so each fragment's page is
fetched one fragment ahead of its count, to keep the misses overlapped
as they were when every page was fetched before the first was counted.

For the A/B the walk is sunrpc's own, with a copy of
`rx_loan_region_span()`, so that only `sunrpc.ko` changes and the boot
is kept (`rx_loan.c` is built in).  If kept, the change belongs in
`net/core/rx_loan.c` (`rx_loan_prefetch_pages()` returning the count),
which takes a kernel build.

**A** is the canary's own sunrpc source, built the same way
(`build-sunrpc.sh`, `$L/sunrpc-PC-A`, srcversion
`BF7C91691266656E8821694`); B is `$L/sunrpc-PC-B`
(`306DE3D63221C6367E625E0`).  Both built without a warning and with
every export's version unchanged, so the canary's own nfsd and the rest
load against either.  The KUnit suites were not run (they took tardis2
down on build 506).

## The runs ([run328.sh](run328.sh))

A B A B A B (`t2-r328..r333`), each as round 74's, the held mark off.
