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

## 1. The tip counted and profiled (r89)

[run89.out](run89.out): the tip's sunrpc `DDE22984B5905541BB2CD35` went in at 19:45 and is the kept sunrpc from here. Five runs, every one with its flows placed, no drive stepping down, and none in a puppet run.

| run | GB/s | nfsd cycles a WRITE | nfsd instructions | ring cycles | ring instructions | peak |
|---|---|---|---|---|---|---|
| r89-1 | 40.57 | 297.0k | 303.2k | 139.9k | 311.7k | 6315 |
| r89-2 | 40.49 | 297.6k | 303.3k | 138.7k | 309.6k | 6494 |
| r89-3 | 40.51 | 297.2k | 303.3k | 138.6k | 310.9k | 6500 |
| r89-4 | 40.48 | 296.4k | 303.1k | 139.9k | 312.5k | 6561 |
| mean | 40.51 | **297.1k** | 303.2k | 139.3k | 311.2k | 6468 |
| profile (55 s) | 40.50 | 296.1k | 303.3k | 138.8k | 310.7k | 6448 |

Against the same day's runs without the second request (run88's A side, 301.1k nfsd cycles), the tip is about 4k cycles a WRITE lower, as run87's A/B said. The ring, the rate and the peak are as they were.

The profile ([perf-lmiss-nfsd-r89.txt](perf-lmiss-nfsd-r89.txt), [perf-sym-nfsd-r89.txt](perf-sym-nfsd-r89.txt), and the rings' beside them), against round 37's run80:

| frame | nfsd's L3 miss samples, r80 → r89 | nfsd's cycles, r80 → r89 |
|---|---|---|
| `svc_tcp_rx_release_refs` | 2.70% → 1.59% | 1.14% → 1.04% |
| `page_pool_put_netmem_bulk` | 1.52% → 0.77% | 0.75% → 0.60% |
| `svc_tcp_rx_region_span` | 1.99% → **2.18%** | 0.55% → 0.57% |
| `svc_tcp_rx_skb_region_pages_after` | 1.79% → **1.60%** | 0.51% → below the list |
| `svc_tcp_rx_append_ref` | 1.35% → 1.30% | 1.57% → 1.45% |
| `svc_tcp_rx_prefetch_pages` | 1.14% → 1.13% | 0.96% → 0.88% |

Round 38's change shows where it should: the release's misses fall by 40% and the pool's bulk put's by half, since the struct pages it reads are now in place. The region count's two frames are now nfsd's first two named frames by L3 misses after the spin lock (3.8% together), which is what step 2 looks at.

## 2. The receive side's struct page loads (r90)

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

**run90** ([run90.out](run90.out), the readouts as `lat-*-r90.txt` and `stat-*-r90.txt`): the run at 40.45 GB/s, its flows placed, no drive stepping down, the look from 20:10:34 (10 s into the timed load). The script's summary line for the section's mean read the wrong line (`sections`, not `section n`) and printed nothing; fixed in run90.sh, and the readouts are whole. About 905k sections in each 2 s window, 11.7 a WRITE, 7.4 region pages each.

| fetch before the count | section, timed whole (1) | nfsd cycles a WRITE, untimed | stall_backend_mem a WRITE |
|---|---|---|---|
| prefetchw (the tip's) | **419 ns** | 291.5k | 92.6k |
| none | 620 ns (+201) | 301.6k (+10.1k) | 99.5k |
| a load | 507 ns (+88) | 295.8k (+4.3k) | 96.5k |
| prefetchw again | **417 ns** | 290.0k | 91.4k |

Each page's `pp_magic` load, drained (2), mean ns, against the floor (a line of the CPU's own) of 33 ns:

| fetch | 1st | 2nd | 3rd | 4th | 5th–8th | 9th–16th | pool's `rx_region` |
|---|---|---|---|---|---|---|---|
| prefetchw | 33 | 31 | 31 | 32 | 32 | 32 | 32 |
| none | 63 | 78 | 77 | 79 | 78 | 76 | 32 |
| a load | 33 | 31 | 31 | 31 | 32 | 32 | 32 |

- **The receive side's prefetches are not dropped.** Unlike the release's, they do their work. Without them the section takes 48% longer and nfsd about 10k more cycles a WRITE. A load in their place is worse than the prefetch (+88 ns a section, +4.3k cycles a WRITE), since a load holds the core while it waits.
- **The struct pages are not in DRAM.** Unfetched, a page's line costs 30 to 46 ns above the floor, mostly in the 64–127 ns bucket. That is a line the ring's CPU wrote, fetched from its cache, not a DRAM miss.
- **The pool's `rx_region` line is a hit** (at the floor every time). It is out of step 3.
- **What is left is the lead.** With the prefetch the section still takes 419 ns, 11.7 times a WRITE, about 4.9 us a WRITE. The struct pages are asked for just before they are read, and the skb's fragment array, which the prefetch walk reads first, is the ring CPU's line too. A request one skb earlier, for the next skb's fragment array and struct pages, could hide most of it without holding any page longer.

## 3. A lead for the receive side's fetch (r91): not kept

Section 2 left the fetch's lack of lead: each loaned skb asks for its struct pages just before its region count reads them. A request one skb earlier holds no page longer, so it cannot raise the region's peak; the peak is reported beside it all the same.

[The A/B](sunrpc-ab-receive-next.patch) (`751A649B7263BAC9E031BFC`: the tip's `DDE22984B5905541BB2CD35` with section 2's diagnostic and `svc_tcp_rx_rcv_next`, not for keeping; only `sunrpc.ko` and `build.log` kept, in `/var/tmp/zc-run-logs/t2-r39-next`). In `svc_tcp_recv_skb()`, before a loaned record's skb is counted, it looks at the skb queued after it on the socket's receive queue:

- 0: nothing (the tip's);
- 1: prefetchw of the next skb's struct pages (its fragment array read for it);
- 2: as 1, and a prefetch of the fragment array (four lines from `skb_shinfo()`) of the skb after that, so that the next skb's walk finds it.

The next skb's own `struct sk_buff`, and in 2 the one after it, are still read on demand. It runs only on a plain TCP socket: there the actor's skb is on `sk_receive_queue`, which does not change under the socket lock. A TLS socket's is not. The fetch is inside the section that the diagnostic times, so a look counts its cost.

[run91.sh](run91.sh): first a look as run90's, with each setting in turn (0, 1, 2, then 0 again): a 2 s `perf stat` untimed and a 2 s window with the section timed whole. Then the A/B, counted and untraced: twelve loan64 runs, four rounds of 0, 1 and 2 with the order rotated each round. The tip's sunrpc goes back in at the end. About 55 minutes.

**run91, stopped after seven of its twelve counted runs (Mike: "stop the current run and just call it not kept")** ([run91.out](run91.out)). The look and every counted run had their flows placed and no drive stepping down. r91-3-n0 was stopped mid-load and is not counted. The client's mounts were taken down and the tip's sunrpc put back by hand, then `settled.sh apply` ran ("the settled setting").

| `svc_tcp_rx_rcv_next` | section (look) | untimed nfsd cycles a WRITE (look) | counted nfsd cycles a WRITE | counted nfsd instructions a WRITE |
|---|---|---|---|---|
| 0 (the tip) | 422, 423 ns | 291.2k, 290.9k | 299.6k, 302.0k (300.8k) | 308.8k, 310.8k (309.8k) |
| 1 | 499 ns | 293.0k | 302.8k, 302.1k (302.5k) | 313.6k, 313.5k (313.6k) |
| 2 | 480 ns | 293.0k | 301.5k, 302.1k, 301.1k (301.6k) | 312.3k, 314.0k, 312.1k (312.8k) |

The lead makes the section 60 to 80 ns slower, not faster, and costs about 1 to 2k cycles and 3 to 4k instructions a WRITE. Asking for the next skb's struct pages means reading its `struct sk_buff` and fragment array at once, lines the ring's CPU wrote too: the miss moves one skb earlier and is paid all the same. The rate and the peak did not move. **Not kept.** (The counted runs are about 3.5k cycles and 6.5k instructions a WRITE above run89's, for the diagnostic this module carries; the same in each arm.)
