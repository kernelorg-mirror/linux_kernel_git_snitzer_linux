# Round 36: nfsd's own loan frames

**The plan, from the queue:** the three frames that loan a WRITE's pages on nfsd: `svc_tcp_rx_append_ref` (each borrowed fragment into the record's bvecs), `svc_tcp_rx_borrow_segment` (the walk's call for each fragment) and `svc_tcp_rx_release_refs` (the pages back to their pool once the write is done). Round 31 found the append to be many short loads, not one stall, so this round looks at the three together.

**The release's timing is fixed.** The pages are the WRITE's data, so they can go back to the pool only after the write. Only what each release costs can change.

**All on tardis2 at the current state:** nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), xeu round 35's (`63631721F8B149913C0DC19`), `rodata=noalias`, the settled setting, and the tip's sunrpc (`A06FE478658242A10E100FF`) unless named. Runs are in `/var/tmp/zc-run-logs/t2-r75/` to `t2-r77/`. Cycles are converted at the cores' 2.0 GHz.

## 1. The look (r75)

Driver [run75.sh](run75.sh): one loan64 run at 40.5 GB/s, on nfsd's CPUs (8 to 55), from 5 s after the WRITEs pass 10,000 a second:
- `perf stat` of cycles, instructions and the cache events ([stat-nfsd-r75.txt](stat-nfsd-r75.txt));
- 4 s of samples each of cycles, backend memory stall cycles and L1D read refills ([samples-cycles-r75.txt](samples-cycles-r75.txt), [samples-stall_backend_mem-r75.txt](samples-stall_backend_mem-r75.txt), [samples-l1d_cache_refill_rd-r75.txt](samples-l1d_cache_refill_rd-r75.txt)), annotated;
- last, bpftrace kprobe counts of each frame's calls ([calls-r75.txt](calls-r75.txt)).

**A WRITE on nfsd's CPUs:** 290k cycles, 301k instructions, 32% of the cycles stalled on memory.

| frame | cycles a WRITE (sampled) | calls a WRITE |
|---|---|---|
| `svc_tcp_rx_append_ref` | 3.7k | about 140 |
| `svc_tcp_rx_borrow_segment` | 3.2k | 131 (r77's count) |
| `svc_tcp_rx_release_refs` | 2.6k | 1, about 70 pages, 68 `put_run`s |
| `svc_tcp_rx_prefetch_pages` | 2.1k | |
| `svc_tcp_rx_walk_skb` | 2.0k | |

Together the loan frames are about 23k cycles a WRITE, 7.8% of nfsd's.

**Where the samples sit** ([borrow](annotate-stall-svc_tcp_rx_borrow_segment-r75.txt), [append](annotate-stall-svc_tcp_rx_append_ref-r75.txt), [release](annotate-stall-svc_tcp_rx_release_refs-r75.txt)):
- **The borrow:** about 35% just after two loads of the same word, the page's `compound_info`, read once by `compound_head()` and then again as `pp_magic` (both at +8).
- **The append:** about 20% on `page->pp` and then the pool's `rx_region` (pool+968), from the region test; about 14% on the coalesce test's bitmap reads.
- **The release:** about 30% on the `compound_head()` load, the first touch of each struct page since the write.

## 2. The changes

The three checks a borrowed fragment goes through each did more work than they needed to:

1. **The append.**
   - The region test now checks whether the page is the one just counted (`state->region_last`) before it looks up the page's pool. Every second fragment of a page cut into two 8 KiB buffers is that page.
   - The coalesce test compares the last bvec's page and offset first, and reads its two bitmaps only when they match.
2. **The borrow.** `svc_tcp_rx_page_is_pp()` answers `page_pool_page_is_pp(compound_head(page))` with one load. For a page that is its own head, as a receive buffer is, `compound_info`'s bit 0 is clear, and the same word is the page's `pp_magic`.

No page is held longer, and nothing new is touched.

**As measured,** the A/B's sunrpc (`D396F4A3FDA305DA0A0756C`, [its patch](sunrpc-lean-append-ab.patch)) had both changes behind an A/B-only `svc_tcp_rx_lean_append`, read at each call.

## 3. The A/B (r76)

[run76.sh](run76.sh): the A/B's sunrpc swapped in, A `svc_tcp_rx_lean_append` N (the tip's), B Y, four untraced loan64 runs a side, A B A B A B A B, then the tip's sunrpc put back. Every run had no drive stepping down and dmesg no WARNING; the end was the settled setting. All lines: [cost-r76.txt](cost-r76.txt).

| run | lean | GB/s | rings µs (tick) | ring cycles / instructions a WRITE | nfsd µs (tick) | nfsd cycles / instructions | peak |
|---|---|---|---|---|---|---|---|
| 1-A | N | 40.47 | 41.9 | 137.6k / 306.1k | 89.6 | 295.8k / 307.5k | 6366 |
| 1-B | Y | 40.57 | 41.5 | 136.7k / 304.7k | 89.3 | 294.1k / 303.5k | 6377 |
| 2-A | N | 40.48 | 42.1 | 137.4k / 306.3k | 89.5 | 295.8k / 308.1k | 6589 |
| 2-B | Y | 40.45 | 43.9 | 137.9k / 305.6k | 92.5 | 295.0k / 304.9k | 6324 |
| 3-A | N | 40.45 | 43.6 | 137.5k / 306.1k | 89.3 | 294.9k / 307.2k | 6404 |
| 3-B | Y | 40.45 | 45.5 | 138.9k / 307.6k | 92.1 | 293.8k / 304.9k | 6444 |
| 4-A | N | 40.56 | 42.3 | 137.7k / 306.6k | 91.6 | 293.8k / 306.6k | 6461 |
| 4-B | Y | 40.50 | 44.6 | 137.6k / 306.0k | 91.2 | 295.6k / 305.1k | 6264 |

| B − A, four pairs | by pair | mean |
|---|---|---|
| nfsd instructions a WRITE | −4.0k, −3.2k, −2.3k, −1.5k | **−2.75k ± 0.54** |
| nfsd cycles a WRITE | −1.7k, −0.8k, −1.1k, +1.8k | −0.45k ± 0.77 |
| ring cycles a WRITE | −0.9k, +0.5k, +1.4k, −0.1k | +0.23k ± 0.49 |
| GB/s | | 0.00 |
| region peak | +11, −265, +40, −197 | not raised |

- **The instructions fall in every pair,** by about 10 a call over the appends and borrows.
- **The cycles do not follow beyond the spread.** Three pairs fell by 0.8 to 1.7k; pair 4 rose by 1.8k.
- **The tick-sampled µs** read B worse on both (+1.4 rings, +1.3 nfsd), where the counted cycles do not (round 35's finding again).

## 4. Where the borrow's wait went (r77)

[run77.sh](run77.sh), [the diagnostic](sunrpc-diag-struct-page-loads.patch): the A/B's sunrpc with a per-CPU timing of the struct page loads (`6F7B01243426DBA783BAE7E`, not for keeping), one loan64 run at 40.48 GB/s, no drive stepping down, no WARNING. On nfsd's CPUs, first four 3 s windows with the timing off, the knob N Y N Y, each a `perf stat` and a `perf record` of cycles at once; then two 3 s windows with the timing on, N then Y.

### The loads, timed

Each load was timed by the 1 GHz counter, drained (`DSB LD`) before and after, as round 35 timed xeu's. About 116k WRITEs a window ([lat-N-r77.txt](lat-N-r77.txt), [lat-Y-r77.txt](lat-Y-r77.txt)):

| load | a WRITE | mean, N | mean, Y | 64 ns or more |
|---|---|---|---|---|
| the borrow's first load of a page's word | 79 | 31.8 ns | 31.9 ns | 0.6% |
| the borrow's load of the page it timed last | 52 | 31.7 | 31.7 | 0.0% |
| a line of the CPU's own (the floor) | | 31.8 | 31.9 | 0.3% |
| the release's first load of a page's word | 78 | 44.2 | 44.3 | 12.3% |

- **The borrow's struct page loads are L1 hits.** The first touch of a page costs what a line of the CPU's own does: `svc_tcp_rx_prefetch_pages()`'s `prefetchw()` has the line in place before the borrow reads it.
- **So the look misread the borrow.** Its 35% of samples after the two loads was not memory wait. It was a serial chain of near loads, as with xeu's refill in round 35. There was no far miss to move.
- **The release is where struct page lines miss.** Its loads take 12.4 ns a page above the floor, even with round 24's prefetch a batch ahead. Timed with barriers, that is at most about 1 µs (about 2k cycles) a WRITE before overlap.
- **Two CPUs' lines are missing.** The parameter's print stopped at its 16 KiB page, so CPUs 54 and 55 and CPU 53's last slot are not in the files. The other 46 CPUs' counts match their histograms.

### The cycles, sampled

One sample every 200k cycles ([w1-N](samples-cycles-w1-N-r77.txt), [w2-Y](samples-cycles-w2-Y-r77.txt), [w4-Y](samples-cycles-w4-Y-r77.txt); annotated borrow [N](annotate-borrow-w1-N-r77.txt), [Y](annotate-borrow-w2-Y-r77.txt)):

| symbol | samples, N (w1) | Y (w2, w4) | Y − N, a WRITE |
|---|---|---|---|
| `svc_tcp_rx_borrow_segment` | 2867 | 2350, 2389 | **−0.86k cycles** |
| `svc_tcp_rx_append_ref` | 2937 | 3021, 2960 | +0.09k |
| the release, the walk, the prefetch | | within ±60 samples | about 0 |
| `default_idle_call` and `finish_task_switch` | 62,028 | 62,640, 62,649 | +1.06k |
| the 80 symbols ranked, net | | | −0.45k |

- **The borrow's saving is real and stays saved.** Nothing else on the WRITE's path took up its cycles.
- **The saved time goes to sleep.** At 40.5 GB/s the four drives are 100% busy, so nfsd does the same WRITEs either way. Its threads finish sooner and sleep more.
  - `default_idle_call` is 29% of nfsd's counted cycles under load.
  - At rest the counter stops in WFI (about 1M cycles a second a CPU), so those cycles are spent going into and out of idle, not inside WFI. More sleeping costs cycles there.
- **The net −0.45k is r76's mean,** which is why the counted totals barely moved.
- **w3 is set aside.** It ran at the same time as `twoport.sh`'s own `perf stat` window and counted a third of the cycles ([stat-w3-N-r77.txt](stat-w3-N-r77.txt)). So N has one window against Y's two.

## Decision

- **Kept (Mike, 28 October),** with no parameter: nfsd about 2.75k instructions a WRITE fewer and the borrow about 0.86k cycles cheaper, with the region's peak not raised. On this drive-limited rig the saving shows as idle, not as rate.
- **The commit:** `eae183aaaff3` "SUNRPC: Cut the loads of a borrowed page's checks" ([the patch](SUNRPC-cut-the-loads-of-a-borrowed-pages-checks.patch)). The coalesce test's page and offset comparison, now made first, is dropped from its return.
- **Installed** on 28 October: built against -devel (`E80412A536ADD1E86B16F5A`, `$L/t2-r36-kept/`), put in place by `swap-sunrpc.sh` with no client connected, and settled.
- **The initrd** was rebuilt with it and swapped in, carrying sunrpc `E80412A536ADD1E86B16F5A` and xeu `63631721F8B149913C0DC19`. The one before is beside it as `initrd.pre-sunrpc-r36-2026-10-28`.
- **Not run:** the svcsock KUnit suite on the kept module. It undoes the settled setting, so `settled.sh apply` would follow it.
- **Next:** the release's struct page misses, the real ones left in the loan frames (12% of its first touches at 64 ns or more). Its prefetch distance and the order it gives pages back in are where to look.
