# Round 45: what holds the receive region's pages

**The ask (Mike, 29 October):** round 43's run109 left the 32 MiB region full with the loan budget at 720 pages, so something besides the loaned WRITEs holds region pages under loans; count what does.

**What there is from before:**

- **Round 43 (run104, run109).** Copy mode's peak is 1168 to 1248 pages at every region size. Loan mode's is 6306 at 512 MiB with a budget of 8198, 4779 at 100 MiB, 3436 at 64 MiB, and all 2048 at 32 MiB with a budget of 1016 or 720. At 720 the rate fell to 21.84 GB/s, lower than 1016's 27.67, with only 6 failed allocations. The sum of copy mode's peak and the budget, about 1890 pages at 720, does not explain a full region.
- **What the region counts.** `/sys/kernel/debug/page_pool_rx_region` gives the free pages, the lowest that reached, and the failed allocations. A page is in use from when a pool takes it until it is given back to the region, whoever has it.
- **Where pages can be, then:**
  1. **Posted on a ring,** waiting for a frame. xeu cuts its 8 KiB buffers two to a 16 KiB page; each ring has 256 entries, so about 128 pages a ring, 512 for port a's four.
  2. **Kept by a ring's page pool for reuse.** Each pool has an alloc cache of up to 32 pages at 16 KiB (`PP_ALLOC_CACHE_SIZE`), and a region stack of the pool's size, 256, since `settled.sh` holds `rx_region_stack` at 0. A page returned by nfsd goes onto its pool's stack, and back to the region only when the stack is full. So up to 4 x 288 = 1152 pages can sit idle in the pools and still count as in use. Round 04 is the same mechanism the other way: a stack of 1024 raised the peak by about 900 pages.
  3. **In the NFS sockets' receive queues,** out-of-order queues and backlogs: records arrived and not yet taken by nfsd, loaned or copied.
  4. **On loan,** held by admitted WRITEs until their write to the drive finishes. The budget charges each 126 pages.
  5. **Elsewhere, briefly:** skbs held by GRO, which now holds them up to 10 µs with round 44's deferral; skbs being received; denied WRITEs being copied.

  The sockets' spare pages (`svc_tcp_spare_bytes_max`) are the threads' own pages, not the region's, so they are not on the list.

## 1. The breakdown, sampled (r110)

[run110.sh](run110.sh), with a diagnostic xeu ([xeu-diag-region-held.patch](xeu-diag-region-held.patch) on round 41's kept xeu, `7CFF720BA3CE35894DD922E`). It adds a read-only parameter, `region_held`, giving for each receive ring the buffers posted to the card, what is left of the page being cut, and its page pool's alloc cache and stack. These are read without the ring's NAPI, so each is a snapshot that may be a poll old.

From 10 s into each timed load, every 0.2 s for 10 s, one sample of:

- the region's free pages;
- the rings' posted buffers, and the pools' cache and stack (`region_held`);
- the NFS sockets' receive and backlog bytes (`ss -tm`: `r` and `bl`);
- the budget's charged credit, `budget - avail - reserve`, and the records in flight (`svc_tcp_rx_budget_stats`).

In pages: posted buffers count two to a page, and the sockets' bytes 16 KiB to a page (each 8 KiB buffer is charged as its truesize, so this is near, not exact). What the region has in use beyond the four is "the rest": GRO and skbs in flight, and whatever the loans hold beyond their charge.

Four runs at the settled setting (round 44's deferral included), counted and untraced, `WARM=10 RUNTIME=30`, after five samples at rest:

| run | region | mode |
|---|---|---|
| r110-1 | 512 MiB | copy |
| r110-2 | 512 MiB | loan64 (budget 8198) |
| r110-3 | 64 MiB | loan, 23 WRITEs (budget 3032), as run104 |
| r110-4 | 32 MiB | loan, budget 720, as run109 |

Then the region at 512 MiB again, the kept xeu and the settled setting.

**What it decides:** if the pools hold several hundred pages under loans and few in copy mode, the region's footprint can be cut at the pools, through `rx_region_stack` (a smaller stack gives pages back to the region sooner, at the cost of mapping them again) or the pool's size. That would be step 2, an A/B judged on the peak, the ring's cycles and the rate. If the queues hold them, the receive buffer under loans is the place to look; if the rest does, a diagnostic sunrpc that counts each loaned record's region pages.

### What r110 found

[run110.out](run110.out), the samples in [r110/](r110/), at 19:48 to 20:07 on 29 October. As run, the samples summed `region_held` over both xeu ports. Port b's eight rings of 4096 entries are up but idle and outside the region (no stack), and showed exactly 32768 posted buffers and nothing cached in every sample, so [held-a.sh](r110/held-a.sh) takes them off. run110.sh now reads port a alone. "The loans" is what is in use less the rings, the pools and the queues; at rest and in copy mode that remainder is 1 and 13 pages.

Mean of the 50 samples, pages (MiB):

| run | GB/s | in use | posted on the rings | pools: stack | pools: cache | sockets' queues | the loans | records on loan | a record holds | the budget charged |
|---|---|---|---|---|---|---|---|---|---|---|
| at rest, 512 MiB | | 528 (8.3) | 504 | 0 | 23 | 0 | | | | |
| copy, 512 MiB | 40.79 | 1133 (17.7) | 493 | 499 | 29 | 99 | (13) | | | |
| loan64, 512 MiB | 40.58 | 5544 (86.6) | 487 | 481 | 28 | 245 | 4303 (67.2) | 63.8 | 67 | 7997 |
| loan 23, 64 MiB | 37.77 | 2774 (43.3) | 492 | 489 | 28 | 285 | 1481 (23.1) | 23.9 | 62 | 2826 |
| loan, budget 720, 32 MiB | 21.57 | 2006 (31.3) | 502 | **654** | 30 | **492** | 328 (5.1) | 5.9 | 56 | 529 |

The runs' own lines: copy 137.8k ring and 678.6k nfsd cycles a WRITE, peak 1152 pages; loan64 129.7k and 295.3k, peak 6436; 64 MiB 128.9k and 267.6k, peak 3490 of 4096 with no failed allocation and 16 retransmits; 32 MiB 141.9k and 226.6k, 2048 of 2048, one failed allocation.

- **A loaned record holds about 1 MiB of region, 56 to 67 pages, not the 126 the budget charges it.** The charge is about twice what is held. So the loans are not what fills a small region: at 32 MiB they held 328 pages of 2006.
- **The pools keep about 500 pages idle in every mode,** 44% of copy mode's in use, and 654 at 32 MiB, up to 741. These are pages returned to a pool and not yet reused: a ring takes from its own pool's stack before the region, so they are not lost to it, but they are to the other three rings.
- **The rings hold about 500 posted, as at rest.** The ring's 256 entries a ring, two buffers to a page.
- **The sockets' queues hold 99 pages copying, 245 to 285 loaning, and 492 at 32 MiB,** where the rate fell by half: records wait in the queues when there is little to loan and nfsd is idle (3% busy in run109).
- **So the 32 MiB region is full of fixed holders, not loans:** about 500 posted, 680 in the pools and 490 queued leave about 330 for loans, five records, which carries 21.6 GB/s. Round 43's reading that a loaned WRITE holds about 98 pages was the peak over the loans, and counted these holders too.
- **The 64 MiB run did not fill its region** (3490 of 4096 at most, no failed allocation) and still lost 7% of the rate. Its limit is likeliest the budget's 23 records at once, not the region; not shown.

**What it means for the footprint (the L3).** Of the region's working set in copy mode, about 1130 pages, half is the pools' idle stacks. In loan mode the stacks are 9% of it. `rx_region_stack` sets their size (0 is the ring's 256); a smaller stack gives pages back to the region sooner, where any ring can take them, and they are mapped again when taken. That is step 2: an A/B of `rx_region_stack` (for example 256 against 64) on the peak, the rings' cycles and the rate, in copy and loan mode. Separately, the budget's charge of 126 pages a WRITE is twice what a loan holds, so a budget sized by it admits half the loans the region could hold.

## 2. The depth of the pools' stacks (r111)

[run111.sh](run111.sh): `page_pool.rx_region_stack` at 0 (the pool's size, the ring's 256; the settled setting), 64 and 16, read when a pool is made, so `settled.sh apply` opens port a again for each (`EXP_RX_REGION_STACK`). Round 41's kept xeu, the region at 512 MiB, the settled setting with round 44's deferral, counted and untraced: loan64 in the order A B C C B A, then copy C B A. Each run reports the region's peak and its mean in use (the free pages every 0.2 s for 10 s from 10 s into the timed load), beside the rate and the rings' and nfsd's cycles.

A smaller stack sends more returned pages back to the region, under its one lock, and a page taken from the region is mapped for DMA again. That is the cost to look for in the rings' cycles, and in nfsd's, whose release returns the loaned pages. A depth is kept only if it lowers the peak without costing rate or cycles beyond the noise.

### What r111 found

[run111.out](run111.out), the samples in [r111/](r111/), at 20:33 to 21:12 on 29 October.

| run | mode | stack | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak, pages | mean in use, pages |
|---|---|---|---|---|---|---|---|
| 1 | loan64 | A, 256 | 40.49 | 130.3k (285.7k) | 297.1k (303.5k) | 6401 | 5413 |
| 2 | loan64 | B, 64 | 40.49 | 134.2k (297.1k) | 307.7k (315.9k) | 6101 | 4889 |
| 3 | loan64 | C, 16 | **32.53** | 123.5k (307.9k) | 272.8k (332.5k) | 5884 | 1757 |
| 4 | loan64 | C, 16 | 40.53 | 141.4k (313.4k) | 326.8k (332.6k) | 6049 | 4989 |
| 5 | loan64 | B, 64 | 40.56 | 133.9k (297.4k) | 306.1k (314.7k) | 6325 | 5152 |
| 6 | loan64 | A, 256 | 40.54 | 130.0k (285.1k) | 295.7k (302.1k) | 6336 | 5342 |
| 7 | copy | C, 16 | 40.79 | 144.5k (285.4k) | 713.5k (480.7k) | 1036 | 678 |
| 8 | copy | B, 64 | 40.80 | 137.8k (269.0k) | 697.2k (462.6k) | 1067 | 774 |
| 9 | copy | A, 256 | 40.80 | 137.5k (269.1k) | 697.6k (463.0k) | 1152 | 1132 |

| | loan64: cycles a WRITE, ring / nfsd | loan64: mean in use | copy: cycles a WRITE, ring / nfsd | copy: mean in use |
|---|---|---|---|---|
| B against A | **+3.9k / +10.5k** (+3.0%, +3.5%), instructions +11.6k / +13.1k | **−357 (5378 to 5021)**, peak −156 | **+0.3k / −0.4k**, instructions the same | **−358 (1132 to 774, −32%)**, peak −85 |
| C against A | +11k / +30k (run 4) | −389 (run 4) | +7.0k / +15.9k | −454 (−40%) |

- **C is not kept.** In run 3 one ring ran at 45% busy against 18% for the other three and held the port at 32.5 GB/s from 8 s into the load, with the drives 78 to 90% busy and no drops ([drives-sec-r111-3-loan64-C.tsv](r111/drives-sec-r111-3-loan64-C.tsv)); in run 4 it cost 11k ring and 30k nfsd cycles a WRITE.
- **B in copy mode is free:** a third less of the region in use, the cycles and instructions the same.
- **B in loan mode costs 3 to 3.5%** of the rings' and nfsd's cycles for 357 fewer pages (5.6 MiB, 7%). Instructions rise with the cycles, so it is work, not waits: a page that finds its pool's stack full is unmapped, its DMA index taken out of the pool's xarray, and put on the region's free list under its one lock (`page_pool_return_netmem()`, `page_pool_rx_region_put()`); a ring that runs short takes it back under the same lock and maps it again (`page_pool_rx_region_alloc()`, `page_pool_dma_map()`). nfsd returns the loaned pages, so it pays most. In copy mode the pages are freed on the ring's own CPU and do not, it seems, overflow the stack as often; this is inferred, not counted.
- **The rate is the same at A and B in both modes.**

Whether B is kept is Mike's: free in copy mode, 3% in loan mode, for 360 pages either way. What it costs in loan mode is a code question too: how many pages a WRITE make the round trip through the region, and which of its steps (the region's lock, the xarray, the mapping) dominates, is for a profile of A against B; a cheaper round trip would make a shallow stack free in loan mode as well.

## 3. Where B's cycles go in loan mode (r112)

[run112.sh](run112.sh): four loan64 runs, A B B A, the kept xeu at the settled setting. From 10 s into each timed load, 5 s of `perf record` on CPUs 0 to 55, one sample every million cycles with call chains, so that a function's samples with its callees are its cycles a WRITE, on the ring CPUs and nfsd's apart; then 5 s of bpftrace counting calls into the steps of the round trip (`page_pool_put_netmem_bulk`, `page_pool_recycle_ring_bulk`, `page_pool_return_netmem`, `page_pool_rx_region_put`, `page_pool_rx_stack_refill`, `page_pool_rx_region_alloc`, `page_pool_dma_map`) by the same groups. The profile names the step that costs (the region's lock and its contention, `__xa_erase` and `__xa_alloc`, the DMA map and unmap), the counts how many pages a WRITE go round. The runs' own cycles are perturbed by both, so run111's stay the A/B.

