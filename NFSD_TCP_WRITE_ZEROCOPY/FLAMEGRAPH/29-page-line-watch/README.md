# Round 29: who writes a region page's struct page line: a look, nothing to change

**The question, from round 27:** `svc_tcp_rx_borrow_segment` spends about 35% of its samples on `page_pool_page_is_pp(compound_head(page))`, about 0.7 µs a WRITE. Loading the next skb's `struct page`s one skb ahead did not move that stall. Round 27 proposed that the line is lost again before the walk reaches it.
- **The suspect:** each 16 KiB page holds two of xeu's 8 KiB buffers, and the frames of 16 connections interleave in the ring. So the two halves of a page often belong to different connections, read by different nfsd threads at about the same time.
- **Why it could not be checked then:** the Neoverse N2 has no SPE to show which write takes the line.

## The tool: hardware watchpoints on one page's line

- **Where the struct pages are:** the region's `struct page`s are at a fixed place. On this host the vmemmap starts at `0xffffff7fc0000000` and `memstart_addr` is `0x80000000`, so the region's `phys_base` `0x8160000000` puts them from `0xffffff8040e00000`.
- **Choosing a page:** [pickpage.py](pickpage.py) reads them through `/proc/kcore` and picks, at random, a region page now in a ring with both halves posted (`pp_magic` set, `pp_ref_count` 2). At rest 1,547 region pages are in the four rings.
- **The watchpoints:** perf puts four on its 64-byte line (`perf record -a -g -e mem:0x<addr>/8:<rw>`, one hardware watchpoint register each on every CPU):
  - writes to `flags` (+0), `pp_ref_count` (+40), and `_mapcount`/`_refcount` (+48);
  - reads and writes of `pp_magic` (+8), the word `compound_head()` and `page_pool_page_is_pp()` read.
  - Every hit is recorded with its CPU, time and call chain.

**Run 2026-10-27 on tardis2**, build 499, the settled setting (xeu `rx_prefetch` Y). The driver is [run56watch.sh](run56watch.sh), the run `/var/tmp/zc-run-logs/t2-r53/`.
- **sunrpc:** the tip's (round 26's module, `A06FE478658242A10E100FF`, the code at the tip since round 27 is not kept), with nfsd the kernel's own. Build 499's own sunrpc was loaded again after.
- **The run:** one loan64 run at 40.48 GB/s. From 5 s after the WRITEs passed 10,000 a second, four pages in turn, each picked then and watched for 3 s.
- **The record:** 109k accesses ([watch.txt](watch.txt); counted by [watch-count.py](watch-count.py), [watch-before.py](watch-before.py) and [watch-first.py](watch-first.py)).
- perf resolved some addresses in sunrpc to wrong `do_perf_trace_*` names; the caller beside them places them.

## What the line sees

For one page (w1, 2.8 s), the most frequent accesses:

| accesses | field | CPU | function |
|---|---|---|---|
| 5,039 | `pp_magic` read | nfsd | `svc_tcp_rx_borrow_segment` |
| 5,025 | `pp_magic` read | ring | `napi_build_skb` |
| 2,623 | `pp_magic` read | nfsd | `svc_tcp_rx_region_span` |
| 2,556 | `pp_magic` read | nfsd | `svc_tcp_rx_release_refs` |
| 2,509 | `pp_ref_count` write | ring | `page_pool_alloc_frag_netmem` |
| 2,201 | `pp_magic` read | nfsd | `svc_tcp_rx_append_ref` |
| 2,125 | `pp_magic` read | ring | `skb_gro_receive` |
| 989 | `pp_ref_count` write | nfsd | `svc_tcp_rx_put_run` |

`flags`, `_mapcount` and `_refcount` were never written. The other three pages look the same.

**The last write before each of the 18,604 reads in `svc_tcp_rx_borrow_segment`:**

| last write | reads | median time before |
|---|---|---|
| the ring CPU, `page_pool_alloc_frag_netmem` (posting the page's halves) | 17,145 (92%) | 253 µs |
| the same nfsd CPU, `svc_tcp_rx_get_borrowed` | 955 (5%) | 3 µs |
| another nfsd CPU, `svc_tcp_rx_get_borrowed` or `napi_pp_put_page` | about 480 (2.6%) | 78 to 103 µs |

**The access just before each of those reads:** another read on the same CPU in 99% of them, 1 to 2 µs earlier, by `svc_tcp_rx_region_span`, `svc_tcp_rx_append_ref` or `svc_tcp_rx_borrow_segment` itself.

**The order of touches on a page:**
1. The ring CPU writes the line when it posts the page.
2. The ring reads it in `napi_build_skb` and GRO.
3. One nfsd thread's first touch is in `svc_tcp_rx_region_span`, a median 20 µs after the ring's last access.
4. The same thread reads it in `svc_tcp_rx_borrow_segment` and `svc_tcp_rx_append_ref` 1 to 2 µs later.
5. A thread releases it in `svc_tcp_rx_release_refs` a median 1.1 ms later.

## What it shows

- **No other CPU takes the line between the first touch and the borrow.** The two halves of a page do not fight over it: the other half's owner touches it about 1.1 ms later, at the release. Round 27's proposal is refuted, and there is nothing to change in xeu's buffer layout.
- **The line has one writer before nfsd reads it, the ring.** The ring-to-nfsd transfer is paid once, at the first touch in `svc_tcp_rx_region_span` (0.59 µs self in fl52).
- **Still unexplained:** why the profiles charge about 35% of `svc_tcp_rx_borrow_segment` to a read that follows the same thread's own read of the line by microseconds.
  - **A timing artefact?** Each watchpoint hit traps, which stretches the gaps. Without traps, the first touch may come well before the borrow (when the record is admitted) and the line may leave the cache in between.
  - **A misattributed sample?** The cycles may belong to a neighbouring instruction.
  - **Settling it:** a count of L1 and L2 refills at that instruction would settle it.

## Decision

- **A look, nothing to change (Mike, 27 October).**
- The watchpoint method, `pickpage.py` with perf's `mem:` events, is kept for later rounds: it shows who writes a line where SPE cannot.
