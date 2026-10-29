# Round 44: NAPI's deferral of interrupts, and where the rings read from DRAM

**The ask (Mike, 29 October):** after round 43, the `napi_defer_hard_irqs` A/B as round 44, and with it the chase for where the rings' ~1.06k DRAM reads a WRITE come from.

**What there is from before:**

- **The interrupts (round 42's r106).** The card has no interrupt coalescing, and raises 9.89 receive interrupts a WRITE. 5.14 of them start a poll with work. 1.82 are stale: pending while the ring was masked, they arrive 0.8 µs after the unmask for work already taken. 2.93 arrive while NAPI already has the ring, so the poll runs again. Timed, the empty polls cost about 1.7k ring cycles a WRITE; with the interrupts' entries, perhaps 2–3% of the rings' cycles.
- **The DRAM reads (round 42).** r100 and r101 counted about 1.06k `ll_cache_miss_rd` and 2.3k lines from beyond the L2 a WRITE, on CPUs 0–7 together. Rounds 42 (r102 to r105) and 41 (r97) timed the lines the poll and GRO read first, and none of them come from DRAM in any number.
- **The profile (round 37's r80).** It put 40% of the rings' L3-miss samples in GRO's frames. Round 41 showed GRO's own loads hit, so those samples are likeliest xeu's prefetch of the next frame, outstanding while GRO runs. Nothing has yet split the rings' misses between the RX CPUs (0, 2, 4, 6) and the TX CPUs (1, 3, 5, 7).

## 1. NAPI's deferral of the receive interrupts (r107)

With `napi_defer_hard_irqs` N and `gro_flush_timeout` T, a poll that completes does not unmask. NAPI polls again from a T timer, and unmasks only after N polls in a row find nothing. xeu masks and unmasks around `napi_complete_done()`'s answer, so it takes this as it is. Fewer interrupts should mean fewer stale and lost ones, at the price of up to T of latency to a burst's first frame and the timer's own cost.

[run107.sh](run107.sh): round 41's kept xeu at the settled setting, nine loan64 runs, counted and untraced, in the order A B C C B A A B C:

| arm | `napi_defer_hard_irqs` | `gro_flush_timeout` |
|---|---|---|
| A | 0 | 0 (the settled setting) |
| B | 2 | 10 µs |
| C | 2 | 30 µs |

Each run reports, beside the usual:

- the RX interrupts a WRITE over the run;
- the region's peak in use, by the footprint rule: frames waiting up to T before a poll hold their buffers longer.

Both settings go back to 0 after each run and at the end. A setting is kept, in `settled.sh`, only if it saves ring cycles without raising the peak or costing rate.


### What r107 found

[run107.out](run107.out), the figures in [r107/](r107/). The first attempt stopped after its first run because the client had another server's mount ([run107-stopped.out](run107-stopped.out)). This is the second, in full, at 18:23 to 19:02 on 29 October.

| run | arm | GB/s | ring cycles a WRITE | nfsd cycles a WRITE | RX interrupts a WRITE | peak in use, pages / MiB |
|---|---|---|---|---|---|---|
| 1 | A | 40.47 | 139.7k | 298.0k | 10.70 | 6606 / 103.2 |
| 2 | B | 40.45 | 130.6k | 295.7k | 1.27 | 6321 / 98.8 |
| 3 | C | 40.46 | 122.3k | 296.9k | 0.09 | 6433 / 100.5 |
| 4 | C | **35.57** | 117.7k | 283.9k | 0.02 | 6373 / 99.6 |
| 5 | B | 40.49 | 130.0k | 295.3k | 1.22 | 6333 / 99.0 |
| 6 | A | 40.50 | 139.6k | 297.7k | 10.63 | 6356 / 99.3 |
| 7 | A | 40.53 | 141.2k | 298.3k | 10.67 | 6541 / 102.2 |
| 8 | B | 40.46 | 130.1k | 295.8k | 1.27 | 6336 / 99.0 |
| 9 | C | **39.33** | 122.4k | 298.0k | 0.03 | 6446 / 100.7 |

| arm | GB/s | ring cycles a WRITE | RX interrupts a WRITE | peak, pages | runs with a stretch at 34 GB/s |
|---|---|---|---|---|---|
| A, 0/0 | 40.50 | 140.2k | 10.67 | 6356 to 6606 | 0 of 3 |
| B, 2/10 µs | 40.47 | **130.2k (−10.0k, −7.1%)** | 1.25 | **6321 to 6336** | 0 of 3 |
| C, 2/30 µs | 40.46, 35.57, 39.33 | 122.4k (runs 3 and 9) | 0.05 | 6373 to 6446 | **2 of 3** |

- **The interrupts were costing more than round 42 timed.** B takes 8.5 times fewer (10.67 to 1.25 a WRITE) and saves 10.0k ring cycles a WRITE. Its three runs agree to within 0.6k. Round 42's timed empty and stale polls came to about 1.7k, so most of the saving is in the interrupts' entry and exit, and in polls that do less work each.
- **B costs no rate and does not raise the peak.** B's three peaks, 6321 to 6336 pages, are under all three of A's, so the footprint rule is met. nfsd's cycles are unchanged (295.6k against 298.0k).
- **C saves more cycles but stalls.** In runs 4 and 9 the four drives fell from about 40.5 to 34 GB/s for 8 to 13 s at a time. They were only 79 to 96% busy then, so less was arriving ([drives-sec-r107-4-loan64-C.tsv](r107/drives-sec-r107-4-loan64-C.tsv), [drives-sec-r107-9-loan64-C.tsv](r107/drives-sec-r107-9-loan64-C.tsv)). There were no FIFO or ring drops and no more retransmits than in the other runs. No run of A or B shows such a stretch. Under C the rings run almost entirely from the 30 µs timer, with 0.02 to 0.09 interrupts a WRITE. Run 4 was first put down to the client and was taken for an outlier; run 9 showed it is C's. Run 9's 10 s window missed its stretch, so its cycles are clean. C is not kept.
- **The ring µs a WRITE did not fall from B to C** (34.2 and 34.2 in runs 2 and 3), although the cycles did. That is consistent with C's rings waiting on the timer, not with less work.

**Proposed: B in `settled.sh`,** `napi_defer_hard_irqs=2` and `gro_flush_timeout=10000` on `enp161s0f0`, and on `enp164s0f0` for the two-port setting once a two-port run has shown the same. That is Mike's to decide; the setting is unchanged.

## 2. Where the rings read from DRAM (r108)

[run108.sh](run108.sh): the kept xeu at the settled setting, one loan64 run. From 10 s into its timed load:

1. **Per CPU on 0–7, counted:** two 3 s windows, of `ll_cache_miss_rd`, `l2d_cache_refill`, `l2d_cache_lmiss_rd`, `l1d_cache_refill`, then `stall_backend_mem`, `bus_access_rd`, `mem_access_rd`, `l3d_cache_refill`. These are summed for the RX CPUs and the TX CPUs apart, a WRITE. Whether the DRAM reads are the receive side's or the transmit side's: the TX CPUs complete the ACKs and the NFS replies, whose skbs were built elsewhere.
2. **Sampled:** 5 s of `ll_cache_miss_rd`, one in 5000, on CPUs 0–7 with call chains, by symbol for each group.
3. **The same with `rx_prefetch` N,** so that no prefetch of the next frame is outstanding while GRO runs. The samples then land nearer the loads that miss, and the remainder is not xeu's prefetch. `settled.sh` puts `rx_prefetch` back.

A sample still lands on the instruction running when the counter overflows, and the core's own prefetcher's misses are counted too, so a symbol's share is where misses are outstanding, not proof of whose load they are. What the split and the two profiles name is then for a diagnostic of the round 41/42 kind, timing those lines at their first touch.

### What r108 found

[run108.out](run108.out), the figures and the profiles in [r108/](r108/). This is one loan64 run at the settled setting, at 19:02 on 29 October: 40.44 GB/s, 139.2k ring cycles a WRITE, peak 6266 pages. A 1 MiB WRITE at MTU 7792 is about 136 frames.

**Counted, by CPU group** ([stat-sum.txt](r108/stat-sum.txt)):

| a WRITE | RX CPUs (0, 2, 4, 6) | TX CPUs (1, 3, 5, 7) |
|---|---|---|
| cycles | 137.2k | 25.8k |
| `ll_cache_miss_rd` | 0.9k | 0.1k |
| `l2d_cache_lmiss_rd` | 0.9k | 0.1k |
| `l3d_cache_refill` | 2.4k | 0.2k |
| `stall_backend_mem` | 25.9k (19% of the cycles) | 5.1k |

The ~1.0k DRAM reads a WRITE are the receive side's: 0.9k on the RX CPUs, about 6.6 for each frame. The TX CPUs take 0.1k.

**Sampled, the RX CPUs,** as demand misses a WRITE from the sample counts (one in 5000):

| | `rx_prefetch` Y | `rx_prefetch` N |
|---|---|---|
| all, a WRITE (a frame) | 785 (5.8) | 976 (7.2) |
| `xeu_rx_refill` | 77 | 80 |
| `xeu_rx_poll` | 76 | 69 |
| GRO (`dev_`, `inet_`, `tcp4_`, `tcp_`, `skb_gro_receive`, `gro_list_prepare`, the flush) | 240 | 257 |
| `eth_type_trans` | 24 | **111** |
| `napi_build_skb`, `__build_skb_around` | 34 | 66 |
| `_raw_spin_unlock_irqrestore` (where masked samples land) | 43 | 46 |

**xeu's prefetch is not the source of GRO's samples.** It removes about 190 demand misses a WRITE, mostly the frame's first line: `eth_type_trans` 111 down to 24, and the skb's build 66 down to 34. GRO's samples stay where they are with it off (257 against 240), so they are GRO's own loads, not a prefetch outstanding while GRO runs. That corrects round 44's starting guess, carried from round 41.

**The loads, from the annotation** ([annotate-pf-rx.txt](r108/annotate-pf-rx.txt); a sample lands one instruction past the load):

- **`xeu_rx_refill`, about 0.57 a frame:** almost all of them on one load, of offset 32 of `struct page`, `dma_addr`, for the buffer being put back on the ring. Each refill reads the `struct page` of a page last used far back in the region. Round 42's r103 prefetched it a refill ahead and saved no cycles, so the miss is there but its latency is mostly hidden.
- **`xeu_rx_poll`, about 0.56 a frame:** the completion entry's first read after its ownership bit and `dmb oshld`. The card writes each 64 B completion to its own line, so it misses in every case.
- **`skb_gro_receive`:** a read of `skb_shared_info.nr_frags`, which lives in the buffer's tail.
- **`inet_gro_receive`, `gro_list_prepare`:** reads of the IP header (`protocol`, at offset 9) and of the held skb's headers in the comparison. These are header lines other than the first, which xeu's prefetch does not cover.

So about 5.8 lines a frame come from DRAM with the prefetch on: the buffer's `struct page`, the completion, the shinfo in the buffer's tail, and the header lines past the first. All of them belong to pages and lines cycled through a region of about 100 MiB in use, three times the 32 MiB L3. Each is one load of a known field, and is for a diagnostic of the round 41/42 kind to time at its first touch. The 19% of the RX cycles stalled on memory bounds what removing them could give.
