# Round 38: the release's struct page misses, and the region count's

**The plan (Mike, 28 October):** the release's struct page misses, which round 36 found (12% of its first touches at 64 ns or more despite round 24's prefetch a batch ahead), with the region count's two frames, `svc_tcp_rx_region_span` and `svc_tcp_rx_skb_region_pages_after`, which round 37's profile put close behind the release in nfsd's L3 misses.

All runs are on tardis2, build 499 with `rodata=noalias`, nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), xeu round 35's (`63631721F8B149913C0DC19`), the settled setting, one loan64 run a time on port a onto the four DPRP5s. The tip's sunrpc is `E80412A536ADD1E86B16F5A` (round 36's). Runs are in `/var/tmp/zc-run-logs/t2-r81/` to `t2-r84/`.

## 1. The look (r81)

[run81.sh](run81.sh), as round 36's `run75.sh`: one loan64 run at 40.47 GB/s, no drive stepping down, no WARNING. On nfsd's CPUs, 4 s each of samples of cycles, backend memory stall, L1D read refills and L3 long-latency read refills, ranked and annotated, then 4 s of call counts ([calls-r81.txt](calls-r81.txt)).

| a WRITE | calls | cycles (sampled) |
|---|---|---|
| `svc_tcp_rx_release_refs` | 0.83 (some calls inlined) | 2.6k |
| `svc_tcp_rx_region_span` | 106 | 1.3k |
| `svc_tcp_rx_skb_region_pages_after` | (inlined into its callers' count) | 1.2k |
| `svc_tcp_rx_append_ref`, `svc_tcp_rx_borrow_segment` | 101 each | 3.9k, 3.3k |

- **`svc_tcp_rx_region_span`'s misses** ([annotated](annotate-l3d_cache_lmiss_rd-svc_tcp_rx_region_span-r81.txt)): about half its L1 and L3 miss samples are just after the load of the page's `pp_magic`, and 20 to 25% after the pool's `rx_region` (pool+968). It reads the struct page that `svc_tcp_rx_prefetch_pages()` asked for just before, and then the pool's line.
- **`svc_tcp_rx_skb_region_pages_after`'s** sit on the skb's fragment array (each fragment's offset and length), not on a struct page.
- **The release's** ([annotated](annotate-l3d_cache_lmiss_rd-svc_tcp_rx_release_refs-r81.txt)): about 36% just after the first load of each struct page (`compound_head()`'s, at +8), about 10% just after the load of the bvec entry a batch ahead whose page it prefetches, and about 10% on the bitmaps.

## 2. The region test from the pfn (r82)

[The diagnostic](sunrpc-diag-region-pfn-range.patch) (`79862C0AE4A8118E0FF3635`, not for keeping): `svc_tcp_rx_region_span()` decides a page is the region's from its pfn against the region's range (`svc_tcp_rx_region_pfn`, `svc_tcp_rx_region_pfns`, from debugfs `page_pool_rx_region`), reading no struct page and no pool. A page that has left the region (escaped, 0 on this rig) would be counted, so the count stays an upper bound. A, the range 0 (the tip's test); B, the range set.

[run82.sh](run82.sh), as round 36's `run76.sh`: four untraced loan64 runs a side, A B A B A B A B.

| B − A, four pairs | by pair | mean |
|---|---|---|
| nfsd cycles a WRITE | +1.1k, −1.2k, −0.7k, −1.9k | −0.68k ± 0.64 |
| nfsd instructions a WRITE | +1.5k, −0.2k, −0.5k, −1.7k | −0.23k ± 0.66 |
| ring cycles a WRITE | −0.2k, −0.7k, +1.1k, −0.7k | −0.12k ± 0.42 |
| GB/s | 40.44 to 40.59 both sides | 0.00 |
| region peak | +67, +163, +10, +146 | +96 ± 36 |

- **Not resolved:** nfsd about 0.7k cycles a WRITE cheaper, inside the spread, as the look's 1.3k for the whole frame bounds it.
- **The peak:** higher in every pair, by less than round 36's pairs swung (+11 to −265). The count is the same on both sides with no page escaped, so admission and the pages' return are unchanged, and no mechanism is known. Not kept on this A/B; a profile of the two sides in one run would show whether the frame's samples fall.

## 3. Where in the release the misses fall (r83)

[The diagnostic](sunrpc-diag-release-by-position.patch) (`7F6A60512D78EEFE9110CFE`, not for keeping) times each release's first load of each distinct page's struct page by the page's ordinal in the release, as round 36 timed them (the 1 GHz counter after ISB, drained by DSB LD before and after), summed over all CPUs. [run83.sh](run83.sh): one loan64 run at 40.57 GB/s, no drive stepping down, no WARNING, three 3 s windows with the timing on ([w1](lat-w1-r83.txt), [w2](lat-w2-r83.txt), [w3](lat-w3-r83.txt)); the three agree to a few tenths of a ns.

- **A release:** 121 entries over 72.6 distinct page-pool pages, 1.12 releases a WRITE, so about 81 pages a WRITE.

| first load of a page, by ordinal | a WRITE | mean | 64 ns or more |
|---|---|---|---|
| 0 | 1 | 36.3 ns | 3.9% |
| 1 to 7 (the floor) | 7 | 31.5 | 0.2 to 0.6% |
| 8 to 15 | 8 | 33.1 rising to 36.1 | 1.7 rising to 4.7% |
| 16 to 31 | 16 | 40.7 | 9.4% |
| 32 to 63 | 32 | 61.4 | 28.6% |
| 64 and on | 17.6 | 65.6 | 33.0% |
| every entry past the first batch | 71.7 | 55.7 | 23.4% |
| the bvec entry a batch ahead, at a line's start | 30.5 | 31.6 | 0.3% |

- **The head is not where the misses are.** The first pages, prefetched just before the loop with no lead, load at the floor. The hypothesis that the misses are the first batch's is wrong.
- **The misses grow with the page's place in the release:** a third of the pages past the 32nd take 64 ns or more, though each was prefetched a batch ahead.
- **The bvec array does not miss.** The 10% of samples after its load in the look is skid, not a wait on it.
- **Serialised, the release's first loads take about 1.75 µs a WRITE above the floor** (all three windows). The timing drains each load, so this is an upper bound; untimed, the misses overlap.
- **The CPU's own line** (`own`, 44 ns) is no floor here: it is touched once a release and leaves the cache between. Ordinals 1 to 7 are the floor, as round 36's 31.8 ns.

**Not explained yet:** why the first pages are already in place with no lead, and why the deep ones miss with a batch's. A diagnostic that sets the release's prefetch distance (none, 16, 32, 64, all at the start) for each window of one run would separate the two: with no prefetch, whether the head still hits; with a longer distance, whether the deep misses fall.

## 4. The release's prefetch distance (r84)

[The diagnostic](sunrpc-diag-release-prefetch-distance.patch) (`DD3375A439B4C7975DD39F9`, not for keeping) is r83's timing by position, plus `svc_tcp_rx_rel_dist`, the release's prefetch distance in entries:
- 0: no prefetch;
- −1: every entry's page prefetched before the loop;
- otherwise, that many entries ahead, with the first that many prefetched before the loop. The tip's distance is 16.

[run84.sh](run84.sh) does one loan64 run at 40.46 GB/s, with no drive stepping down and no WARNING. For each distance in turn (16, 0, 32, 64, −1, then 16 again), it waits 1 s, takes 2 s of `perf stat` on nfsd's CPUs with the timing off ([stat-*-r84.txt](.)), then 2 s with the timing on ([lat-*-r84.txt](.)). The figures are each page's first load, by its place in the release: the mean, and in brackets the share at 64 ns or more.

| distance | 0 | 1 to 7 | 8 to 15 | 16 to 31 | 32 to 63 | 64 on | excess over 31.6 ns a WRITE (timed) | nfsd cycles a WRITE (untimed) |
|---|---|---|---|---|---|---|---|---|
| 16 (the tip's) | 35.2 (3.0%) | 31.6 (0.2%) | 34.3 (2.9%) | 39.0 (7.8%) | 54.8 (22.5%) | 60.0 (27.8%) | 1.40 µs | 291.4k |
| 0, none | 74.6 (48.7%) | 99.8 (66.1%) | 98.3 (64.0%) | 94.1 (59.9%) | 90.7 (56.1%) | 86.0 (52.6%) | 4.94 µs | 295.9k |
| 32 | 34.6 (2.4%) | 32.0 (0.6%) | 38.1 (6.4%) | 45.2 (13.6%) | 51.1 (18.8%) | 60.4 (27.9%) | 1.42 µs | (lost, below) |
| 64 | 33.9 (1.7%) | 32.1 (0.7%) | 39.7 (7.8%) | 79.6 (45.4%) | 50.1 (17.9%) | 57.2 (25.0%) | 1.90 µs | 292.7k |
| all at the start | 35.7 (3.2%) | 32.1 (0.7%) | 39.3 (7.5%) | 79.2 (45.3%) | 77.3 (43.2%) | 75.7 (42.7%) | 3.08 µs | 295.9k |
| 16 again | 35.4 (3.1%) | 31.6 (0.2%) | 34.4 (2.9%) | 39.1 (7.9%) | 55.1 (22.7%) | 60.2 (28.0%) | 1.42 µs | 290.8k |

- **The tip's distance of 16 is the best of those tried.** 32 ties it on the timed figure. 64 and all-at-the-start are worse, and no prefetch is worst: about 4.5k nfsd cycles a WRITE untimed, and 3.5 µs timed. Nothing in the distance is worth building. The two windows at 16 agree to 0.6k cycles and to a few tenths of a ns.
- **The head is not cached before the release.** With no prefetch, pages 1 to 7 miss two times in three. They hit at 16 because the prefetch issued just before the loop brings them in. One possibility: the timing's DSB LD before the first load may also wait for those prefetches, which would make the head's hits partly the timing's own doing. That is a question, not a finding.
- **A burst of prefetches loses its later ones.** At 64 and all-at-the-start, the pages prefetched more than about 16 to 32 deep in the burst before the loop miss 43 to 45% of the time. That is worse than with a lead of 16 in the loop. More than that many prefetches at once are not all served.
- **The deep misses are not a matter of lead.** At 16, each timed entry spends at least one full drained load, so 16 entries ahead is far more than a DRAM miss. Still, a quarter of the pages past the 32nd miss, and 32 or 64 ahead moves that by only a few points. For those pages the prefetch does not take effect, or the line is taken away again before its use. Which of the two is not known.
- **The untimed window at 32 is lost.** It counted 7,616 WRITEs and 6.6G cycles in 2 s, about a tenth and a third of every other window's. The drives wrote 40.1 to 40.9 GB/s in every second of the run, and the timed window that followed counted 77,777 WRITEs. Neither figure is used.
