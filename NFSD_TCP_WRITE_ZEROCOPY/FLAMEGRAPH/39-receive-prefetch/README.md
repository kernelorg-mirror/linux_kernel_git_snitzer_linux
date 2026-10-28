# Round 39: the receive side's struct page misses

**The plan (Mike, 28 October):** the region count's two frames,
`svc_tcp_rx_region_span` and `svc_tcp_rx_skb_region_pages_after`, and
whether `svc_tcp_rx_prefetch_pages()`'s prefetches are dropped as round 38
found the release's were. The work starts from the tip, counted and
profiled again.

All runs are on tardis2: build 499 with `rodata=noalias`, nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), xeu round 35's (`63631721F8B149913C0DC19`), the settled setting, one loan64 run at a time on port a onto the four DPRP5s. Since r88, `twoport.sh` keeps the client's puppet runs out of a load and refuses a run whose flows cannot be placed (round 38, sections 7 and 9). A look starts with `timed_load` (round 38, section 8). Runs are in `/var/tmp/zc-run-logs/t2-r89/` on.

## Why here

In round 37's profile (run80), the frames where nfsd's L3 read misses land, past idle and the scheduler:

| frame | share of nfsd's miss samples |
|---|---|
| `svc_tcp_rx_release_refs` | 2.70% (round 38's; fewer misses since) |
| `svc_tcp_rx_region_span` | 1.99% |
| `svc_tcp_rx_skb_region_pages_after` | 1.79% |
| `page_pool_put_netmem_bulk` | 1.52% |
| `svc_tcp_rx_append_ref` | 1.35% |
| `svc_tcp_rx_prefetch_pages` | 1.14% |

- **The region count's two frames together miss more than the release did.** Round 38 tried one idea on them, the pfn range test. It was not resolved on CPU, raised the peak by +96 ± 36 for no known reason, and was not kept.
- **`region_span`:** in round 38's look, about half its miss samples come just after its load of the page's `pp_magic`. That is the struct page `svc_tcp_rx_prefetch_pages()` asked for just before, which is the release's pattern before round 38. Another 20 to 25% come after the pool's `rx_region` (pool+968).
- **`pages_after`'s misses** sit on the skb's fragment array (each fragment's offset and length), not on a struct page.

## The steps

1. **The tip counted and profiled** ([run89.sh](run89.sh)). The rig has swapped back to round 36's sunrpc (`E80412A536ADD1E86B16F5A`), which lacks round 38's commit. The tip's, `DDE22984B5905541BB2CD35` (built from `93b10c1877fc` in `t2-r39-tip`), takes its place and stays as the kept sunrpc. Then four untraced loan64 runs, counted as round 37's `run79.sh`, and one with `PROFILE=1`, as `run80.sh`. The release's share should fall, and the ranking is read again from this profile.
2. **The receive side's prefetch, timed** as round 38's r85 timed the release's. In `region_span`, time the first load of each page's struct page with `svc_tcp_rx_prefetch_pages()`'s prefetch as it is, and with a load in its place. If the load takes the misses away, the prefetches are dropped, and round 38's answer (a second, earlier request) is the thing to try. Untimed cycles go beside it, for a load's stall.
3. **Then, by what step 2 and the profile show:** the pool's `rx_region` line, which every page's region test reads, and `pages_after`'s fragment reads.

Every change that holds pages longer reports the region's peak beside its saving, and is not kept if the peak rises.

## 2. The receive side's struct page loads (r90, to run)

The fetch has no lead. In `svc_tcp_recv_skb()`, `svc_tcp_rx_prefetch_pages()` asks for every struct page of the skb's body and then `svc_tcp_rx_skb_region_pages_after()` reads the same pages at once, so the first page's miss has nothing ahead of it to hide in. The release's prefetch had a batch's lead. Here the question is first what the section costs and where, before whether a request is dropped.

[The diagnostic](sunrpc-diag-receive-prefetch.patch) (`D09FD368FE300634356470E`, on the tip's `DDE22984B5905541BB2CD35`, not for keeping; only `sunrpc.ko` and `build.log` kept, in `/var/tmp/zc-run-logs/t2-r39-rcv`) has two knobs.

`svc_tcp_rx_rcv_ahead` sets the fetch before the count:
- 0: prefetchw of each page, the tip's;
- 1: none;
- 2: a load of each page's first word.

`svc_tcp_rx_diag_rcv` sets the timing:
- 1: the section, fetch and count together, timed whole with nothing drained inside it, with its pages and calls counted;
- 2: as 1, and in `svc_tcp_rx_region_span()` each page's `pp_magic` load (by its place in the section: 1st, 2nd, 3rd, 4th, 5th to 8th, 9th to 16th, and on), its pool's `rx_region` load, and a line of the CPU's own as the floor, each drained before and after.

A drain before a timed load waits out the fetches before it, outside the timing. So the section's time (1) is the yardstick, and the loads' (2) say where the time goes. The counts are a per-CPU allocation, as round 38's r85 learned.

[run90.sh](run90.sh): one loan64 run. Its windows start 10 s into the timed load (`timed_load`, the first look to use it). For each fetch in turn (0, 1, 2, 0 again) it waits 1 s, takes 2 s of `perf stat` on nfsd's CPUs untimed, then 2 s at timing 1 and 2 s at timing 2. The tip's sunrpc goes back in at the end.
