# Round 38: the release's struct page misses, and the region count's

**The plan (Mike, 28 October):** the release's struct page misses, which round 36 found (12% of its first touches at 64 ns or more despite round 24's prefetch a batch ahead), with the region count's two frames, `svc_tcp_rx_region_span` and `svc_tcp_rx_skb_region_pages_after`, which round 37's profile put close behind the release in nfsd's L3 misses.

All runs are on tardis2, build 499 with `rodata=noalias`, nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), xeu round 35's (`63631721F8B149913C0DC19`), the settled setting, one loan64 run a time on port a onto the four DPRP5s. The tip's sunrpc is `E80412A536ADD1E86B16F5A` (round 36's). Runs are in `/var/tmp/zc-run-logs/t2-r81/` to `t2-r86/`.

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

## 5. The deep misses (r85)

[The diagnostic](sunrpc-diag-release-deep-misses.patch) (`5EE406BEEF99F01E09A4620`, not for keeping) is r84's, with two additions to tell its two explanations apart: either the prefetch does not take effect, or the line is taken away before its use. The build is in `/var/tmp/zc-run-logs/t2-r38-deep`. Its counts are allocated per CPU when the timing is first turned on, and freed when sunrpc is unloaded. The first build kept them in static per-CPU data, 10,112 bytes, which the 8 KiB reserved for modules' per-CPU data could not hold: it failed to load (`Cannot allocate memory`), and the run's stop path put the tip's sunrpc back, settled.

- **`svc_tcp_rx_rel_ahead`** sets how a page is fetched the distance ahead:
  - 0: prefetchw, the tip's;
  - 1: a prefetch for reading;
  - 2: a load of the struct page's first word, which cannot be dropped as a prefetch can.
- **Each release's distinct pages are classified at its end**, when it has no more than 256 of them:
  - by their place from the end of the release;
  - by whether each is shared: its `pp_ref_count`, read just after the timed load, above this release's references to it;
  - with a load ahead (2), by whether the load ahead and the load at use each took 64 ns or more.

How to read it:
- If the deep misses go with a load ahead, the prefetches were being dropped.
- If they stay, the line was taken away between the load ahead and the use.
- If those pages are also shared, the cause is another CPU writing the line, not an eviction.

[run85.sh](run85.sh): one loan64 run at the distance of 16. For each way of fetching (0, 1, 2, 0 again) it waits 1 s, takes 3 s of `perf stat` on nfsd's CPUs with the timing off, then 3 s with the timing on.

The run went at 40.48 GB/s, with no drive stepping down, no WARNING and no release past 256 pages. Readouts: [lat-*-r85.txt](.), [stat-*-r85.txt](.). Each entry is a page's first load at use: the mean, and in brackets the share at 64 ns or more.

| fetched ahead by | 16 to 31 | 32 to 63 | 64 on | last page | shared, 32 on | alone, 32 on | excess a WRITE (timed) | nfsd cycles a WRITE (untimed) |
|---|---|---|---|---|---|---|---|---|
| prefetchw (the tip's) | 38.5 (7.3%) | 48.4 (16.5%) | 50.4 (18.7%) | 52.3 (21.9%) | 58.9 (26.2%) | 46.7 (15.1%) | 0.99 µs | 296.0k |
| prefetch for reading | 38.6 (7.5%) | 49.2 (17.2%) | 50.5 (18.9%) | 52.3 (21.8%) | 59.8 (27.1%) | 47.1 (15.5%) | 1.02 µs | (window short, below) |
| a load | 31.5 (0.2%) | 31.4 (0.2%) | 31.4 (0.2%) | 31.4 (0.3%) | 31.4 (0.2%) | 31.4 (0.2%) | 0.00 µs | 302.3k |
| prefetchw again | 38.5 (7.4%) | 48.3 (16.3%) | 50.1 (18.5%) | 52.0 (21.7%) | 58.5 (26.0%) | 46.6 (14.9%) | 0.98 µs | 296.9k |

With a load ahead, sorted by whether the load ahead and the load at use each took 64 ns or more, a WRITE:

| ahead → at use | pages | at use |
|---|---|---|
| hit → hit | 28.5 | 31.2 ns |
| miss → hit | 42.9 | 31.2 ns |
| hit → miss | 0.0 | — |
| miss → miss | 0.1 | 162 ns |

The loads ahead themselves: 136 a WRITE, 68.1 ns on average, 36% at 64 ns or more.

- **The deep misses are dropped prefetches.** A load 16 entries ahead brings every page in: 0.2% miss at use, at every place in the release, shared or not. No line a load brought in is taken away before its use (0.1 of 71 pages a WRITE). With a prefetch the same pages miss 15 to 26% of the time, so those prefetches are not served.
- **Asking for the line exclusive is not why.** A prefetch for reading misses the same as prefetchw.
- **Pages that another holder shares miss more often with a prefetch** (26% against 15% past the 32nd), and so does the last page (22%). With a load, they hit like the rest. A shared page's line is more likely to be in another core's cache, but that a prefetch is dropped more often for such a line is a guess, not shown here.
- **A load ahead is not the fix.** Untimed, it costs nfsd about 6k more cycles a WRITE than prefetchw (302.3k against 296.0k and 296.9k): 43 of the 71 loads a WRITE miss, and each holds back the instructions behind it, where a prefetch does not. What would make the prefetches stick, or take their place without stalling, is still to be found.
- **The second window counted short again.** Like r84's, it counted 88,139 WRITEs untimed and 43,032 timed, against about 117,000 and 116,000 in the others. Its release count (48,083 against 130,000) fell with it, while the drives wrote 39.6 to 40.9 GB/s every second and the run's rate was the usual 40.48 GB/s. Its untimed cycles are not used. Its timed shares are per page, and they match prefetchw's. Why nfsd's counts fall in one window of a run is not known.

## 6. Kinds of prefetch (r86)

[The diagnostic](sunrpc-diag-release-prefetch-kinds.patch) (`8F97DEB564CBBF076C97644`, not for keeping; built in `/var/tmp/zc-run-logs/t2-r38-kinds`) is r85's, with four more values of `svc_tcp_rx_rel_ahead`, each looking for a fetch that does not stall the way a load does:
- 3: a prefetch for writing into the L2 only (PSTL2KEEP);
- 4: a prefetch for reading into the L2 only (PLDL2KEEP);
- 5: prefetchw twice per page, 16 entries ahead and again `svc_tcp_rx_rel_dist2` (4) ahead;
- 6: two stages, PSTL2KEEP 32 entries ahead and prefetchw 16 ahead.

[run86.sh](run86.sh): one loan64 run. For each of 0, 3, 4, 5, 6 and 0 again, it waits 1 s, takes 2 s of `perf stat` on nfsd's CPUs with the timing off, then 2 s with the timing on. It stamps each window's start and end, to tell whether a window that counts few WRITEs, as r84's third and r85's second did, is a short one.

The run went at 40.48 GB/s, with no drive stepping down and no WARNING. Readouts: [lat-*-r86.txt](.), [stat-*-r86.txt](.). Each entry is a page's first load at use: the mean, and in brackets the share at 64 ns or more. This run's 2 s windows show fewer misses with prefetchw than r85's 3 s ones (9% against 16% for 32 to 63), so compare within the run.

| fetched ahead by | 16 to 31 | 32 to 63 | 64 on | last page | shared, 32 on | excess a WRITE (timed) | nfsd cycles a WRITE (untimed) |
|---|---|---|---|---|---|---|---|
| 0, prefetchw at 16 (the tip's) | 36.8 (5.7%) | 40.3 (9.0%) | 41.5 (10.2%) | 48.8 (18.4%) | 50.8 (18.6%) | 0.55 µs | 293.3k |
| 3, PSTL2KEEP at 16 | 36.6 (5.1%) | 41.2 (9.4%) | 42.7 (11.0%) | 49.9 (18.3%) | 52.3 (19.1%) | 0.60 µs | 290.1k |
| 4, PLDL2KEEP at 16 | 36.3 (4.9%) | 40.7 (9.0%) | 42.4 (10.9%) | 49.0 (18.1%) | 51.3 (18.7%) | 0.57 µs | (lost, below) |
| 5, prefetchw at 16 and at 4 | 32.7 (1.5%) | 33.6 (2.3%) | 33.4 (2.1%) | 33.6 (2.6%) | 36.7 (5.2%) | 0.12 µs | 293.1k |
| 6, PSTL2KEEP at 32, prefetchw at 16 | 31.9 (0.7%) | 32.6 (1.3%) | 33.0 (1.9%) | 33.0 (1.9%) | 35.0 (3.6%) | 0.06 µs | 290.0k |
| 0 again | 36.9 (5.7%) | 40.2 (8.8%) | 41.5 (10.2%) | 48.8 (18.4%) | 50.6 (18.5%) | 0.54 µs | 294.0k |

- **Two stages take the misses away:** an L2 prefetch at 32 and prefetchw at 16 leave 1 to 2% of the deep pages at 64 ns or more (3.6% of the shared ones), against 9 to 18% with prefetchw alone. The timed excess falls from 0.55 µs to 0.06 µs a WRITE.
- **Prefetching twice nearly does the same,** 2 to 5%. This and the two-stage result both say a page's second request is served when its first was dropped.
- **An L2 prefetch alone misses as prefetchw does,** whether for writing or for reading. An L2 hit would take well under 64 ns, so those misses are prefetches dropped at the L2 too.
- **Untimed, two stages took 290.0k nfsd cycles a WRITE, against 293.3k and 294.0k for the tip's:** about 3.6k cycles, or roughly 1.2 µs, a WRITE less. That is one window against two, which agreed to 0.7k. The L2 prefetch alone also measured 290.1k with no fewer timed misses, and prefetching twice measured 293.1k. One window each is not enough for a decision: an A/B of two stages against the tip's, with several untimed pairs, is what would say whether it is kept. A prefetch holds no page, so the region's peak should not move, but the A/B reports it.
- **The short window again:** the untimed one at 4 counted 1,439 WRITEs and 4.5G cycles where the others counted about 79,000 and 23G. The stamps show it lasted its full 2.04 s, and the drives wrote 39.7 to 40.9 GB/s every second of the run. So nfsd's CPUs did a fifth of their work for those 2 s while the drives' writes did not fall. It has come about 10 to 13 s after the look began in each of r84, r85 and r86, about 30 s into the writes.

## 7. The A/B of two stages (r87, to run)

[The A/B's sunrpc](sunrpc-ab-release-two-stage.patch) (`C45C8709B1251E6570577DF`, built in `/var/tmp/zc-run-logs/t2-r38-two`) is the tip's with `svc_tcp_rx_rel_two_stage`. With it set, the release also prefetches each page into the L2 two batches ahead (PSTL2KEEP), with prefetchw a batch ahead as the tip does. It carries no timing and nothing else of the diagnostics.

[run87.sh](run87.sh), as `run82.sh`: four untraced loan64 runs a side, A B A B A B A B, with A at N (the tip's) and B at Y. `cost.sh` gives each run's ring and nfsd cycles a WRITE, and each run reports the region's peak. A prefetch holds no page, so the peak should not move.

## 8. The short window: the looks start before the timed load

The window that counted few WRITEs (r84's third, r85's second, r86's third) is the gap between two loads, not a stall. Before its timed load, `twoport.sh` runs a placement job (`PLACE`, the default since 11 October). That is 16 s of the same fio job on the same mounts, while it sees where each flow lands and sets the RSS table. Then it audits and starts the timed load.

The looks wait 20 s and then for more than 10,000 WRITEs a second, and the placement job already passes that. So every look started in the placement job, about 13 s before the timed load. Its first windows measured the placement job, the short one fell in the 2 to 4 s between the two jobs, and the rest measured the timed load.

| run | look began | the timed load began (its drives' first second) | windows in the placement job |
|---|---|---|---|
| r81 | 07:19:10 | about 07:19:23 | the first samples |
| r83 | 08:02:48 | about 08:03:00 | all: stat-off, w1, w2, w3 |
| r84 | 15:19:30 | about 15:19:43 | 16, 0; 32's untimed in the gap |
| r85 | 16:10:57 | about 16:11:10 | 0; prefetch for reading's in the gap |
| r86 | 16:27:23 | about 16:27:36 | 0, 3; 4's untimed in the gap |

The drives' per-second figures start with the timed load (`drvsample.py`), which is why they showed no dip. nfsd writes direct to them (`io_cache_write` 2), so a real stall would have shown.

**Round 38's findings stand.** The placement job is the same load with its flows not yet placed, and the same setting measures alike in both. In r86 prefetchw measured 293.3k nfsd cycles a WRITE in the placement job and 294.0k in the timed load, with the same misses by position to a tenth of a ns. In r85 the two prefetchw windows were 296.0k and 296.9k. Comparisons within a run that cross the gap (r84's distance 0 and r86's PSTL2KEEP against windows in the timed load) still mix the two phases, which is one more reason for r87's A/B.

**Earlier rounds:** the same trigger is in the looks of rounds 29 to 36 (`run56watch.sh` to `run77.sh`). r75 and r77, round 36's, also began 13 s before their timed loads, so their first windows were in the placement job too. Their counts a WRITE come from `cost.sh` over `twoport.sh`'s own window in the timed load and are not affected; only what their looks sampled is.

**The fix:** a look should wait for the timed load itself, for its run's `drives-sec.tsv` to appear, and then its warm-up. This is to go into the next look's script and into `common.sh` once r87 is done, since `twoport.sh` reads `common.sh` at each run.
