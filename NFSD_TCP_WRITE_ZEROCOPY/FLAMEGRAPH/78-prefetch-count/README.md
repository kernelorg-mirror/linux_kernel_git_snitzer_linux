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
No new kernel warning, no drive stepped down
([run328.out](run328.out)).  **The put-back at the end left B loaded:**
`swap-sunrpc.sh` with `SUNRPC_KO=none` takes nothing out of `updates/`
(its header says to `rm` the module and `depmod -a` first; rounds 74 and
75 used `STACK=none`, which does).  It was put right by hand at 06:25
(stock `2A2297D3850E37768921BCC` loaded, `updates/` empty, the settled
setting, both ports answering), and `run328.sh` now does both.

| run | | GB/s | nfsd | prefetch + count | far (a, b) | region median, p99, max sampled |
|---|---|---|---|---|---|---|
| `t2-r328` | A | 69.95 | 241.8k | 5951 | 2.6%, 2.9% | 3279, 6377, 9003 |
| `t2-r330` | A | 70.79 | 241.6k | 5872 | 1.6%, 1.5% | 3342, 6639, 9763 |
| `t2-r332` | A | 69.77 | 234.6k | 5840 | 1.3%, 2.0% | 3290, 6528, 9354 |
| `t2-r329` | B | 70.86 | 246.6k | 8168 | 1.8%, 2.0% | 3375, 6548, 8819 |
| `t2-r331` | B | 71.27 | 250.0k | 8325 | 0.9%, 1.6% | 3394, 6525, 8024 |
| `t2-r333` | B | 70.02 | 245.9k | 8261 | 1.9%, 1.8% | 3305, 6591, 10329 |

A's "prefetch + count" is `rx_loan_prefetch_pages` +
`rx_loan_skb_region_pages.part.0` + `rx_loan_region_span`; B's is
`svc_tcp_rx_prefetch_count` + `svc_tcp_rx_region_span`
([fsum.py](fsum.py)).  By function over the three runs of each
([walk-AB.txt](walk-AB.txt)):

| | A | B |
|---|---|---|
| prefetching walk | 3131 (IPC 1.19, 45.2 refills) | 3449 (IPC 1.24, 48.5) |
| count's walk | 1208 (IPC 2.18, 36.3) | in the one above |
| `region_span` | 1549 (IPC 2.09, 50.8) | **4803 (IPC 1.48, 84.3)** |
| the two passes | 5888 | **8251** |
| nfsd, all | 239.4k | 247.5k |
| GB/s | 70.17 | 70.72 |

- **The fold costs 2.4k cycles a WRITE more than it saves.**  The second
  loop's 1.2k is gone, as meant, but the count now waits for the lines
  the walk has just asked for: `region_span`'s L2 refills a WRITE rise
  from 51 to 84 and its cycles from 1.5k to 4.8k.  One fragment ahead is
  too short a lead: in A every page of the skb was asked for before the
  first was read, and A's count already took 51 refills a WRITE with
  that lead.
- **The rate:** B is ahead in each pair (0.91, 0.48, 0.25 GB/s), 0.55 on
  average, with nfsd 8.1k cycles a WRITE more.  As in rounds 74 and 75
  the rate follows the client's far share: the pairs where B led most
  are the ones where A's far share was higher (`t2-r328` 2.6%/2.9%).
  Three pairs cannot tell 0.55 GB/s from that, and nothing in B's
  profile is a reason for it to be faster.
- **The region:** the same (median 3.3k to 3.4k, p99 6.4k to 6.6k).

**Verdict (for Mike):** drop as built; the patch stays on
`kernel-7.3/canary.PC` (`f9eb67570da8`, not pushed), the branch the code
was built from.  What the fold can save at most is the second loop's
1.2k, and only if the count still reads pages fetched a whole skb
earlier; that is two loops again, inside one function.  The misses are
the cost here, not the walking.
