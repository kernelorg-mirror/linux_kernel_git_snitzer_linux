# Round 42: where the rings' stalls are

**The plan (Mike, 29 October).** Steer toward the rings' cost. After round 41 the ring CPUs spend 139k counted cycles a loaned WRITE (`cost.sh`), and what is left there is not instructions: round 41 took 6.5k instructions a WRITE off them and their cycles did not move. Before any change, this round finds where the rings' cycles stall, and on what. The starting point is the current state: build 499 with `rodata=noalias`, the tip's sunrpc (`DDE22984B5905541BB2CD35`), nfsd the kernel's (`3F0C341E86E266D5FDF2ADF`), xeu round 41's (`CA0931345E46C929B11D01F`, `rx_gro_frags` Y), the settled setting, one loan64 run at a time on port a onto the four DPRP5s.

## Why here

Round 41's run99, `perf stat` on the ring CPUs (0–7) with `rx_gro_frags` off, per WRITE:

| | cycles | share |
|---|---|---|
| all | 163.8k | |
| backend stalls (`stall_backend`) | 69.5k | 42% |
| of them on memory (`stall_backend_mem`) | 28.7k | 18% |
| **backend stalls not on memory** | **40.8k** | **25%** |

- **Most of the backend stalls are not cache misses.** On a Neoverse N2, a backend stall that is not on memory is mostly ordering and stores: barriers, the store buffer, MMIO. xeu has those per frame and per poll: `dma_rmb()` on each completion, the refill's barriers and doorbell writes, the interrupt's re-arm. Round 35 found one already: a release atomic waiting for the refill's descriptor stores to drain.
- **The profiles cannot settle it.** tardis2 runs without pseudo-NMI, so every sample taken with interrupts off is charged to where they come back on. `handle_softirqs` shows 8.7% self on the rings and no one can say what it is. Round 41 showed that the L3 misses sampled in GRO were xeu's prefetches in flight.
- **Two precise tools are built but off:**
  - **SPE** (`arm_spe_pmu`, a module not loaded): the exact instruction, its latency and where its data came from.
  - **Pseudo-NMI** (`CONFIG_ARM64_PSEUDO_NMI=y`, needs `irqchip.gicv3_pseudo_nmi=1` and a reboot): samples inside the interrupts-off code.

## The steps

1. **The stalls counted** (no change to the host): in one loan64 run, consecutive 3 s windows of `perf stat` on the ring CPUs, each counter group with cycles and instructions, per WRITE:
   - the top level: `stall_frontend`, `stall_backend`, `stall_backend_mem`, `stall_slot_backend`;
   - barriers and exceptions: `dsb_spec`, `dmb_spec`, `isb_spec`, `exc_irq`;
   - the caches: `l1d_cache_refill`, `l2d_cache_refill`, `l3d_cache_refill`, `ll_cache_miss_rd`;
   - the bus and TLB: `bus_access_rd`, `bus_access_wr`, `remote_access`, `dtlb_walk`;
   - polls and interrupts: `napi:napi_poll` and `irq:irq_handler_entry` counted, then 3 s of `bpftrace` for frames a poll on the rings.
2. **SPE on the ring CPUs for one run** (`modprobe arm_spe_pmu`, Mike's to allow): the instructions and the data the stalls fall on, and where the data came from (L1, L2, the SLC, DRAM, a device's write).
3. **The change step 2 points to**, an interleaved A/B on counted ring cycles, the region's peak beside it. Candidates: fewer barriers or doorbells a frame in xeu (ordering once per batch of completions, not per frame), fewer polls and interrupts a WRITE if a poll's fixed cost is large, the completions and descriptors to the core sooner.

Pseudo-NMI only if SPE cannot account for `handle_softirqs`'s share; it needs a reboot.

**Rules for the round, as for the others:**
- Loan mode only. A change is judged on the rings' counted cycles a WRITE, with nfsd's, the rate and the region's peak beside it. A change that raises the peak is not kept.
- An xeu swap reopens the port and drops the client's connections, so each one is Mike's to ask for.

## 1. The stalls counted (r100)

[run100.sh](run100.sh): one loan64 run at the current state, no module swapped. From 10 s into the timed load, five 3 s `perf stat -C 0-7` windows, one counter group each (1 s apart), the WRITEs in each window counted from `/proc/net/rpc/nfsd`; then 3 s of `bpftrace` on `napi:napi_poll` for each ring poll's frames.

**Run 29 October:** 40.53 GB/s, flows placed, no drive stepping down, counted ring 139.5k and nfsd 296.2k cycles a WRITE, peak 6434, settled at the end ([run100.out](run100.out), the windows in [r100/](r100/)). The windows count the ring CPUs system-wide, so their cycles (164.5k a WRITE) are above `cost.sh`'s. A frame here is about 136 a WRITE.

| a WRITE, on the ring CPUs | count | a frame | share of cycles |
|---|---|---|---|
| frontend stalls | 0.9k cycles | | 0.5% |
| backend stalls | 72.6k cycles | | 44.1% |
| of them on memory | 31.3k cycles | | 19.0% |
| `dsb` / `dmb` / `isb` executed | 65 / 327 / 77 | 0.5 / 2.4 / 0.6 | |
| interrupts taken (`exc_irq`) | 15.5 | | |
| L1D refills / L2 refills / last-level read misses | 1.12k / 1.05k / 1.06k | 8.2 / 7.7 / 7.8 | |
| L3 refills | 2.55k | 18.7 | |
| bus reads / bus writes, in 32-byte beats | 4.59k / 0.62k | 33.7 / 4.6 | |
| data TLB walks | 0.6 | | |
| NAPI polls / interrupt handlers (window 4) | 13.0 / 12.5 | | |

- **The front end is not the problem** (0.5%), and neither is the TLB (since round 30's `rodata=noalias`).
- **The rings read about 2.3k lines a WRITE from beyond their L2, 17 a frame, and about 1.06k of them come from DRAM.** `bus_access_rd` counts 32-byte beats on this core, two to a line, and `ll_cache_miss_rd` counts lines the core's prefetcher brings in as well as demand misses: [`xeu-rig/busread.c`](../../xeu-rig/busread.c), reading 512 MiB once on an idle CPU and counting around the loop alone, read 2.001 bus counts and 1.001 last-level misses a line, and 0.109 L2 refills (the prefetcher's lines are not counted as refills). So of the 4.59k beats, 2.3k lines (about 147 KB a WRITE, 5.7 GB/s): about 1.06k missed the L3 (the CMN's system-level cache, the only cache past a core's L2) and went to DRAM, and about 1.24k hit in it. As first written here this read "4.59k lines, four times the misses, 11 GB/s"; the unit was then checked.
- **What a frame's 17 lines are is not known:** the completion, the two header lines, the `struct page`, the descriptor and software ring entries, the socket and page_pool's state are fewer than that. The core's prefetcher running on past what is read (the headers into the payload) is one possibility.
- **Barriers are few:** half a `dsb` and 2.4 `dmb` a frame. Whether they cost much each is for SPE to show.
- **A poll and an interrupt about every 10 frames:** 13 polls and 12.5 interrupts a WRITE, so most polls start from an interrupt. 62.6% of polls handle 0–3 frames and 4.2% use the whole budget of 64 ([r100/polls.txt](r100/polls.txt), on the ring CPUs, TX polls included; its WRITE count is not used, as the trace's attach fell inside the window). xeu does not offer interrupt coalescing (`ethtool -c`: not supported).
- **25% of the cycles are backend stalls not on memory,** and these counters do not say on what. On this core that is mostly the core's queues being full behind long operations: stores, barriers, the device's MMIO.

**So, for step 2:** where the rings' lines beyond L2 come from, and why about half miss the L3.

## 2. What the rings read beyond L2, and where the card's writes go (r101)

SPE was the plan, and is not to be had: `arm_spe_pmu` loads, but the firmware's ACPI MADT gives no CPU an SPE overflow interrupt (0 on all 64; the PMU's is 23), so the driver creates no device. Making one would take a patched MADT through the initrd (`CONFIG_ACPI_TABLE_UPGRADE=y`), a reboot, and SPE enabled for the kernel by the firmware, which an empty field suggests it is not. The module was unloaded again. In its place (Mike, 29 October), two looks with no change to the host:

1. **The switches within one run,** on the ring CPUs: `rx_prefetch` and `rx_gro_frags` in turn (YY NY YN YY NY YN, 3 s each), counting cycles, instructions, `bus_access_rd`, `l2d_cache_refill`, `ll_cache_miss_rd` and `stall_backend_mem`. Without `rx_prefetch`, the lines the core's prefetcher brings in after a frame's headers should move with it if they are payload; without `rx_gro_frags`, the built skbs' lines come back.
2. **The mesh** (`arm_cmn_0`, the CMN-700's counters, system-wide), at rest and under load, four groups of four in turn:
   - what the PCIe devices write and read: `rnid_s0/s1/s2_wdata_beats` (the card's received frames and completions, mostly) and `rnid_s0/s1/s2_rdata_beats` (the drives reading the payload, the card sending);
   - the L3: `hnf_slc_sf_cache_access`, `hnf_cache_miss`, `hnf_cache_fill`, `hnf_slc_eviction`;
   - DRAM and stashing: `hnf_mc_reqs`, `hnf_stash_data_pull`;
   - snoops: `hnf_snp_sent`, `hnf_sf_hit`, `hnf_sf_evictions`, `hnf_dir_snoops_sent`.

   Whether the card's writes are filled into the L3 (fills against the device's write beats), how much the payload takes of it, and whether anything is stashed. If the card's writes go to DRAM, every completion and header line the rings read costs a DRAM read, and the lever is outside the CPU: the card stashing those lines (CMN supports stashing; whether the E1 can ask for it is Xsight's).

[run101.sh](run101.sh): `busread` on CPU 63 again; the mesh's groups at rest during the rest before the load; one loan64 run with the switch windows from 10 s into its timed load and the mesh's groups under load after them. `settled.sh` puts both switches back.

**Run 29 October:** 40.49 GB/s, flows placed, no drive stepping down, counted ring 139.7k and nfsd 295.7k cycles a WRITE, peak 6419, settled at the end ([run101.out](run101.out), the windows in [r101/](r101/)). `busread` again: 2.001 bus beats and 1.001 last-level misses a line.

**The switches, on the ring CPUs, a WRITE** (two 3 s windows each, system-wide on CPUs 0–7):

| `rx_prefetch`, `rx_gro_frags` | cycles | instructions | lines beyond L2 | L2 refills | LL read misses | memory stalls |
|---|---|---|---|---|---|---|
| Y, Y (the settled setting) | 165.1k, 166.0k | 320.5k, 321.1k | 2.27k, 2.29k | 1.04k, 1.05k | 1.06k, 1.07k | 31.3k, 32.1k |
| N, Y | 168.5k, 168.3k | 311.6k, 312.0k | 3.45k, 3.51k | 0.95k, 0.95k | 1.35k, 1.37k | 37.8k, 37.7k |
| Y, N | 164.8k, 164.6k | 326.4k, 326.8k | 2.36k, 2.36k | 1.00k, 1.00k | 1.08k, 1.08k | 29.0k, 29.0k |

- **`rx_prefetch` keeps its place, and is not pulling in payload.** Without it the rings take 3.3k cycles a WRITE more, 6k more memory stall cycles, and read 1.2k more lines beyond L2, 0.3k more of them from DRAM. The lines beyond L2 are not the core's prefetcher running on behind xeu's prefetch.
- **`rx_gro_frags` may cost the rings a little:** with it off, 0.9k cycles a WRITE fewer here (164.7k against 165.6k), 5.8k instructions more and 2.7k memory stall cycles fewer. Run99 had it the other way round by 0.4k. Over the two runs it is between −0.4k and +0.9k cycles, inside the windows' scatter. It is noted here, not acted on.

**The mesh** (`arm_cmn_0`, system-wide, a WRITE under load less the rest; RN-D beats are 64 bytes here: 16,483 a WRITE is 1.055 MB, a WRITE's payload and headers):

| | a WRITE | a payload line |
|---|---|---|
| devices' writes to memory (the card's frames and completions) | 16.5k lines | 1.00 |
| devices' reads from memory (the drives reading the payload; the card's sends) | 16.5k lines | 1.00 |
| L3 accesses (`hnf_slc_sf_cache_access`) | 49.0k | 2.97 |
| L3 misses | 32.7k | 1.98 |
| L3 fills / evictions | 18.5k / 15.4k | 1.12 / 0.94 |
| DRAM requests (`hnf_mc_reqs`) | 29.3k | 1.78 |
| snoops sent / snoop filter hits | 2.6k / 15.5k | |
| stash pulls | 0 | |

- **The payload goes through DRAM.** About 1.8 DRAM requests a payload line, 1.87 MB a 1 MiB WRITE, about 72 GB/s at this rate. The likeliest reading: the card's writes are filled into the L3 (fills 18.5k against the card's 16.5k lines), and are evicted (15.4k) before the drives read them, so the payload is written back to DRAM and the drives read it from there. The L3 is 32 MiB; at 40 GB/s it holds 0.8 ms of payload, less than a WRITE's life from its arrival to the drive's read. That is the region, by the rule about its footprint, already larger than the L3 (6,419 pages of 16 KiB at peak, about 100 MiB).
- **Nothing is stashed.**
- **The rings' 1.06k DRAM reads a WRITE are not explained by this.** The lines the card has just written, a completion or a frame's headers, are read by the ring within microseconds of the fill, so they should hit in the L3. What the rings miss in DRAM is likelier their own state touched last by another core (page_pool's recycled pages and their `struct page`s, released on nfsd's CPUs; the sockets) or lines evicted between uses. The counters cannot say which.

**So:** the rings' memory stalls (31k cycles a WRITE, 19%) come from about 1.06k DRAM reads and 1.2k L3 hits a WRITE, and the payload's own path through DRAM is the system's, not the rings'. Naming the rings' lines needs either a precise profile (pseudo-NMI, a reboot) or a diagnostic xeu that times the poll's remaining first touches as round 41 timed GRO's: the completion entry, the software ring entry, and the refill's take of a recycled page from page_pool.

## 3. The poll's first touches (r102)

[xeu-diag-poll-first-touches.patch](xeu-diag-poll-first-touches.patch), on round 41's kept xeu (`9461DCE8A67161729215CE0`), adds `poll_lat_every` (0, off, by default). Times are the 1 GHz counter after an ISB; a load is timed with a DSB LD before and after, a call whole:

| | what | how often |
|---|---|---|
| `own` | a line of the CPU's own, just written: the floor (about 32 ns) | one frame in N |
| `cmpl_first` | the completion at a poll's head, when valid | every poll |
| `cmpl_next` | the next frame's completion, where `xeu_rx_prefetch_next()` reads its valid bit: the first touch of every later completion | one frame in N |
| `sw_entry` | the next frame's software ring entry | one frame in N |
| `frame_page` | the next frame's buffer's `struct page`, before the prefetch asks for it | one frame in N |
| `frame_hdr` | the next frame's first header line, before the prefetch asks for it | one frame in N |
| `pp_alloc` | a call of `page_pool_alloc_pages()` in the refill, whole: from the pool's own cache, or a batch taken from its ring of pages returned by other CPUs | one page in N |
| `new_page` | the `struct page` that call returned, at its first load | one page in N |
| `doorbell` | the refill's doorbell, `xeu_ring_db()`, whole | every refill |
| `rearm` | `xeu_ring_irq_rearm()`, the MSI-X unmask, whole | every poll that completes |

The lines are timed at their first touch, before the prefetch, so a time well above the floor is a line coming from the L3 (about 64–127 ns) or DRAM (128 ns or more), the lines the rings would miss without the prefetch or pay for at the poll's head. A sampled frame's lines are warm afterwards, so N is 16 and the run's counted cycles are not a result.

[run102.sh](run102.sh): load the diagnostic xeu (refusing if an NFS client is connected), one loan64 run with sampling for 8 s from 10 s into the timed load, then round 41's kept xeu again and the settled setting.

### What run102 found

[run102.out](run102.out); the per-CPU histograms are [r102/poll-lat.txt](r102/poll-lat.txt), the summary [r102/poll-lat-sum.txt](r102/poll-lat-sum.txt). 309,874 WRITEs in the 8 s window. Per WRITE that is 6.39 polls with a completion, 135 frames, 67.7 pages allocated, 4.57 doorbells and 6.44 re-arms (the per-frame and per-page kinds are sampled one in 16 and scaled). The run's 139.7k ring cycles a WRITE are the kept xeu's 139.3k within scatter, so the sampling cost little; its rate is not a result.

Share of first touches by time (log2 buckets; the floor is 32–63 ns):

| | mean | 32–63 ns | 64–127 ns (L3) | 128–255 ns (DRAM) | 256–511 | 512–1023 | 4–8 µs |
|---|---|---|---|---|---|---|---|
| `own` | 32.7 | 99.9% | 0.0% | 0.0% | | 0.1% | |
| `cmpl_first` | 119.7 | 38.1% | 49.7% | 0.1% | 0.1% | **12.1%** | |
| `cmpl_next` | 34.0 | 97.2% | 2.7% | | | | |
| `sw_entry` | 32.2 | 99.9% | 0.1% | | | | |
| `frame_page` | 34.6 | 96.0% | 3.6% | 0.3% | 0.1% | | |
| `frame_hdr` | 62.8 | 47.8% | 50.8% | 1.0% | 0.4% | | |
| `pp_alloc` (call) | 53.3 | 93.6% | 3.9% | 1.9% | 0.1% | | **0.4%** |
| `new_page` | 83.2 | 21.8% | **74.1%** | 3.0% | 1.0% | | |
| `doorbell` (call) | 32.4 | 99.8% | 0.1% | 0.1% | | | |
| `rearm` (call) | 32.9 | 99.3% | 0.4% | 0.2% | 0.1% | | |

What each costs above the floor, a WRITE, at 2 GHz, if nothing overlaps it:

- **`new_page`, about 6.8k cycles** (50.5 ns × 67.7), 4.9% of the rings' cycles. Three in four `struct page`s that `page_pool_alloc_pages()` hands the refill come from the L3. The page was last written elsewhere: released by an nfsd CPU, or by another ring, and the refill's first load waits for it. Nothing prefetches it today.
- **`pp_alloc`, about 2.8k cycles** (20.9 ns × 67.7). 94% of calls are at the floor, from the pool's own cache. Half the excess is the 0.4% that take 4–8 µs, one in about 230 allocations, which looks like the batch taken from the pool's ring of returned pages.
- **`cmpl_first`, about 1.1k cycles** (87 ns × 6.39). Half the polls find the completion in the L3, which is expected for a line the card has just written. **12% wait 512–1023 ns**, a time no L3 or DRAM read takes. My guess is a read that collides with the card's write to that same line, since completions are smaller than a line and the card writes into a line the CPU holds; not shown. (Corrected on looking: the receive completions are 64 bytes, `XSL_DMA_QE_SIZE_64B`, one to a line, so the card does not write into a line that holds a completion the CPU has read. A collision, if it is one, is with the card's write of the head's own line.)
- **`frame_hdr`**: half the header lines are in the L3 (the card wrote them) and 1.4% further out. They would cost about 8k cycles unhidden (30 ns × 135), which is what xeu's next-frame prefetch hides today; run101's prefetch-off window (+3.4k cycles) says it hides most of it.
- **The completions after the first, the ring entries, the frames' `struct page`s, the doorbell and the re-arm** are at the floor: the doorbell and re-arm are posted writes, and the rest are warm.

**The DRAM reads are not here.** These first touches account for about 136 lines a WRITE coming from beyond the L2 (70 header lines, 53 `struct page`s, the rest a few each) and fewer than 5 from DRAM, against run100's 1.05k L2 refills and 1.06k L3 read misses a WRITE. Most of the rings' misses are elsewhere in the rings: skb and GRO, TCP, the transmit side's ACKs and completions.

### Step 3

1. **Prefetch the refill's `struct page`s (A/B).** In xeu's refill, take the pages for the refill first, `prefetchw()` each `struct page`, then post the descriptors; or prefetch the next page's while posting the current one. The pages, their number and their order are what they are today, so the region's footprint does not change. At most about 6.8k ring cycles a WRITE, less what the core already overlaps.
2. **The completion that waits 0.5–1 µs (diagnostic).** Split `cmpl_first` by whether the poll follows the interrupt or is a re-poll, and time a second read of the same line straight after, to tell a collision with the card's write from something else. It is about 1k cycles a WRITE, so it comes second.

## 4. Prefetching the refill's struct pages (r103)

[xeu-ab-refill-prefetch.patch](xeu-ab-refill-prefetch.patch), on round 41's kept xeu (`D2FAF3EF3E02A99063C6FBC`), adds `rx_refill_prefetch` (default N), switched between runs without a reload. With it on, `xeu_rx_refill()` takes the pages it still needs from the page pool, up to 16 at a time (a refill of 32 buffers is 16 pages), and does a `prefetchw()` on each `struct page` before posting any. The fragment count's store and the DMA address's load then find their lines on the way, and the misses overlap instead of following one another. Both arms take the pages in the batch, so the switch moves only the prefetch.

The pages, their number and the order they are posted in are the same with the switch on or off. Each page is taken from the pool at most a refill's few hundred nanoseconds earlier, so the region's footprint does not change; the peak is reported beside the result anyway.

What to expect: at most the 6.8k ring cycles a WRITE that run102 put on `new_page`, less what the core already overlaps. run98's eight runs put the noise between arms near 0.4k.

[run103.sh](run103.sh): load the A/B's xeu (refusing if an NFS client is connected), eight loan64 runs, counted and untraced, `rx_refill_prefetch` N Y Y N N Y Y N, then round 41's kept xeu again and the settled setting.

### What run103 found

[run103.out](run103.out). All eight runs placed their flows, no drive stepped down, no new WARNING, and the kept xeu and the settled setting were back at the end.

| `rx_refill_prefetch` | GB/s | ring cycles a WRITE | ring instructions | nfsd cycles | peak pages in use |
|---|---|---|---|---|---|
| N (runs 1, 4, 5, 8) | 40.52, 40.51, 40.54, 40.52 | 138.2k, 138.4k, 137.5k, 138.4k | 308.3k, 308.6k, 307.4k, 308.6k | 295.4k, 296.8k, 296.0k, 296.4k | 6320, 6389, 6636, 6585 |
| Y (runs 2, 3, 6, 7) | 40.50, 40.50, 40.54, 40.48 | 138.4k, 137.1k, 136.9k, 138.9k | 311.6k, 309.4k, 308.8k, 312.3k | 296.7k, 295.6k, 295.6k, 295.1k | 6333, 6302, 6480, 6362 |
| mean, N / Y | 40.52 / 40.51 | 138.1k / 137.8k | 308.2k / 310.5k | 296.2k / 295.8k | 6482 / 6369 |

- **No gain to be seen.** The prefetch takes 0.3k ring cycles a WRITE off on average, against noise near 0.4k; by neighbouring pair (Y less N in run order) it is +0.2k, −1.3k, −0.6k and +0.5k. The rate does not move. The batch adds 2.3k instructions a WRITE.
- **So the core already overlaps most of `new_page`'s 6.8k cycles.** The first touch of a page's `struct page` is the fragment count's store and the DMA address's load, and neither holds the refill up for long: the store waits in the store buffer, and the address goes to a descriptor the core does not read back. run102's timing, a load fenced by DSBs, showed what the line costs, not what the refill waits for it.
- **The footprint is unchanged,** as designed: peak pages in use 6369 against 6482, inside the runs' own scatter (6302 to 6636).

**Not kept.** `rx_refill_prefetch` stays out of xeu; round 41's kept xeu (`CA0931345E46C929B11D01F`) is the one loaded.

**Next is step 3's second item,** the completion at a poll's head that waits 0.5–1 µs, which is at most about 1k ring cycles a WRITE. With run102's first touches and run103 together, the rings' 1.06k DRAM reads a WRITE are still not placed: they are not in the poll's first touches, and the one excess that was found there the core already hides.

## 5. The completion at a poll's head (r105)

run102 found 12% of polls waiting 512–1023 ns for the completion at their head, a time no L3 or DRAM read takes. [xeu-diag-poll-head.patch](xeu-diag-poll-head.patch), on round 41's kept xeu (`EDAEF51052A598878DAED83` built), adds `head_lat_on` (0, off, by default). On every receive poll while it is set, it times four loads, each between DSB LDs as before:

| | what |
|---|---|
| `own` | a line of the CPU's own, just written: the floor |
| `prev` | the completion before the head: read by the last poll, not written by the card since. The control |
| `1st` | the completion at the head |
| `2nd` | the same line again, straight after |

Each is split two ways:

- **`irq` or `re`:** whether the poll follows this ring's interrupt (stamped in `xeu_napi_intr()` on the same CPU), or is a poll that used its budget or that NAPI ran again.
- **`irqsoff` or `irqson`:** every other poll does the four reads with interrupts off.

It also gives the time from the ring's interrupt to the start of its poll (`irq_to_poll`), and counts polls whose head is empty (`empty_irq`, `empty_re`).

How to read it:

- If the slow reads are only in `irqson`, they were an interrupt landing inside the timed read: run102's 12% is the measurement's, not the line's.
- If `prev` is slow as well, the wait is not the completion's line: the CPU itself is slow to start (waking from idle, say).
- If only `1st` is slow and `2nd` is at the floor, it is the head's line: a read meeting the card's write of that line.

[run105.sh](run105.sh):

1. Put the region back to the settled 32768 pages. run104 (round 43) was stopped on 29 October with it at 6400; this is done the way run104 resizes.
2. Load the diagnostic xeu, refusing if an NFS client is connected.
3. One loan64 run with `head_lat_on` for 8 s from 10 s into the timed load.
4. Load round 41's kept xeu again and apply the settled setting.

### What run105 found

[run105.out](run105.out); the per-CPU histograms are [r105/head-lat.txt](r105/head-lat.txt), the summary [r105/head-lat-sum.txt](r105/head-lat-sum.txt).

- The region went back to 32768 pages (512 MiB) with 0 pages escaped, and the run was at the settled setting.
- The run: 40.44 GB/s, flows placed, no drive stepping down, peak 6469 pages, ring 141.1k cycles a WRITE (every poll timed, so not a result).
- At the end the kept xeu (`CA0931345E46C929B11D01F`) was back, with the settled setting.
- The window was 8 s, 308,416 WRITEs.

| a WRITE | n/W | mean ns | <64 | 64–127 | 128–255 | 256–511 | 512–1023 | ≥1 µs |
|---|---|---|---|---|---|---|---|---|
| `irq`, interrupts off: `own` | 2.68 | 32.4 | 100% | | | | | |
| `prev` | | 32.4 | 99.9% | 0.1% | | | | |
| `1st` | | 63.3 | 38.7% | 61.2% | 0.1% | | | |
| `2nd` | | 32.3 | 99.8% | 0.2% | | | | |
| `irq`, interrupts on: `own` | 2.68 | 54.8 | 95.6% | | | 0.1% | **4.3%** | |
| `prev` | | 42.4 | 98.0% | 0.1% | | 0.1% | **1.9%** | |
| `1st` | | 77.5 | 36.1% | 61.1% | 0.1% | 0.1% | **2.6%** | |
| `2nd` | | 32.4 | 99.8% | 0.2% | | | | |
| `re`, interrupts off: `1st` | 0.71 | 51.9 | 60.1% | 39.8% | 0.1% | | | |
| `re`, interrupts on: `1st` | 0.71 | 51.7 | 60.7% | 39.2% | 0.1% | | | |
| `empty_irq` / `empty_re` | 2.37 / 1.12 | 32.2 | 100% | | | | | |
| `irq_to_poll` | 7.73 | 1416 | | | 71.0% | 16.7% | 0.1% | 12.2% |

(`own`, `prev` and `2nd` of the `re` polls are at the floor, with or without interrupts.)

- **run102's 12% at 0.5–1 µs was an interrupt landing inside the timed read, not the line.** With interrupts off, no read of the head takes more than 255 ns. With them on, the slow reads appear in every one of the three reads that run first in a poll after an interrupt: `own` 4.3%, `prev` 1.9% and `1st` 2.6%. They start at the floor, and nothing of the line's own makes them slow. They come only in polls that follow an interrupt, so a second interrupt arrives within about a microsecond of the poll's start, in roughly one such poll in eleven. Which interrupt it is is not shown here. It is not the transmit ring's: the RX rings' interrupts are on CPUs 0, 2, 4 and 6 and the TX rings' on 1, 3, 5 and 7 (`/proc/irq/*/effective_affinity_list`), which run106 counts.
- **The head's completion itself costs little.** Three in five after an interrupt come from the L3, as a line the card has just written should, and two in five in a re-poll. The second read is at the floor. Above the floor that is about 31 ns × 5.36 and 20 ns × 1.42 a WRITE, about 0.2 µs, **0.4k ring cycles a WRITE**, against run102's estimate of 1.1k (the excess there was the interrupts'). Nothing to win here.
- **A third of the polls after an interrupt find nothing.** 731,627 of the 2,383,056 polls that follow the ring's interrupt (2.37 a WRITE) find the head empty. With the 1.12 empty re-polls, that is 3.5 empty polls a WRITE against 6.8 that do work. Each costs an interrupt, a NAPI run and an MSI-X unmask for nothing. The likeliest cause is the card raising the interrupt for completions the previous poll had already taken before it unmasked; that is not shown.
- **The interrupt waits for its poll:** 71% start within 128–255 ns, but **12% wait 2–64 µs** (284k of 2.38M), while the CPU is in other softirq work. This is latency to the frames rather than ring cycles, and is noted.

**So step 3 ends with the poll's own first touches accounted for.** The `struct page`'s miss is overlapped already (run103), the head's completion costs about 0.4k cycles, and run102's slow heads were interrupts. The rings' 1.06k DRAM reads a WRITE (run100, run101) are elsewhere: skb and GRO, TCP, the transmit side. Two things worth a look follow from r105:

- **The empty polls.** 3.5 a WRITE: what an empty poll costs, and whether the card raises an interrupt for completions a poll has already taken.
- **The 2–64 µs wait** for one interrupt in eight: what the CPU is doing in it.

The empty polls are step 4 (Mike, 29 October: closure on round 42 first, then run104).

## 6. The polls that find nothing (r106)

[xeu-diag-empty-polls.patch](xeu-diag-empty-polls.patch), on round 41's kept xeu (`4A7F5BFDEEE8F39ACF263F6` built), adds `empty_lat_on` (0, off, by default). The receive poll is wrapped while it is set, and on every poll it records what each question needs.

**Why a poll finds nothing.** The ring is masked from its interrupt until `napi_complete_done()`, then unmasked. A completion the card writes in that window leaves its MSI-X vector pending. If the poll then takes that completion before the unmask, the unmask sends an interrupt for work already done.

- `unmask_to_irq_empty` and `unmask_to_irq_work`: the ring's last unmask to its next interrupt, for interrupts whose poll finds the head empty and for those that find work. If the empty ones are stale they come within a microsecond or so of the unmask, and the others are spread out.
- `wait_valid_irq` and `wait_valid_re`, on every other empty poll: a spin of up to 4 µs for the head to turn valid (`never_valid_*` when it does not). An interrupt that arrives before its completion can be read would turn valid within nanoseconds. A stale one waits for the next frame, about 0.8 µs at this rate, or longer.
- Counts of the re-polls' causes:
  - kicks from the interrupt or the refill timer that NAPI had already taken (`lost_*`, the `NAPI_STATE_MISSED` path);
  - polls that used their budget;
  - `napi_complete_done()` returning false.

**What an empty poll costs.** On the empty polls that do not spin, the time from the poll's start to its return (`empty_poll_*`), and from the interrupt's handler (`empty_irq_whole`). Neither includes the interrupt's entry before `xeu_napi_intr()` or `net_rx_action()`'s own work. Polls that find work are timed as well, for scale.

**Which interrupts the ring CPUs take.** `/proc/interrupts` and `/proc/softirqs` on CPUs 0–7 over the same window, a WRITE, to name r105's second interrupt.

[run106.sh](run106.sh): load the diagnostic xeu (refusing if an NFS client is connected), one loan64 run with `empty_lat_on` for 8 s from 10 s into the timed load, then round 41's kept xeu again and the settled setting. The spin delays one empty poll in two by up to 4 µs, so the run's rate and cycles are not a result.

### What run106 found

[run106.out](run106.out). The summary is [r106/empty-lat-sum.txt](r106/empty-lat-sum.txt), the per-CPU counts and histograms [r106/empty-lat.txt](r106/empty-lat.txt), and the interrupts [r106/irq-sum.txt](r106/irq-sum.txt). The run: 40.50 GB/s, flows placed, no drive stepping down, peak 6388; ring 139.3k cycles a WRITE (not a result, as the empty polls spun). The window was 8 s, 309,632 WRITEs, 135.4 frames a WRITE. At the end the kept xeu and the settled setting were back.

**Where a WRITE's 9.89 receive interrupts go** (2.46–2.49 a WRITE on each of CPUs 0, 2, 4 and 6, and nothing else on them: no IPIs, no timer):

| | a WRITE | |
|---|---|---|
| the interrupt starts a poll that finds work | 5.14 | |
| the interrupt starts a poll that finds the head empty | 1.82 | `unmask_to_irq_empty`: 96% 512–1023 ns after the ring's unmask, mean 0.80 µs |
| the interrupt comes while NAPI already has the ring (`lost_irq`) | 2.93 | sets `NAPI_STATE_MISSED`: `napi_complete_done()` returns false 2.92 times a WRITE, and the poll runs again |

**And the polls, 10.54 a WRITE:** 6.96 started by an interrupt, and 3.58 run again: 2.92 for those lost interrupts and 0.67 after using their budget. The refill timer does not count (0.002). 7.16 find work, and 3.38 find nothing (1.82 after an interrupt, 1.56 run again).

- **The empty polls after an interrupt are stale interrupts.** The ring is masked from its interrupt to the end of its poll. A completion the card writes in that time leaves the vector pending. The poll takes that completion, unmasks, and the pending message arrives about 0.8 µs later for work already done: 96% of the empty polls' interrupts come 512–1023 ns after the unmask, against a spread out to 8 µs and more for interrupts that find work. In the spun half, 70% of the heads did not turn valid within 4 µs, and the rest mostly took 1–4 µs: the next frame, not a late one. This holds whether or not the poll before the unmask had done work (0.80 µs either way).
- **The lost interrupts are the card's interrupts sent before the mask lands.** The card has no interrupt coalescing (`ethtool -c`: not supported), so while the ring is unmasked it raises one per completion. The mask is written from the handler of the first, and a frame arrives on a ring about every 0.8 µs at this rate, so a further one gets out before the mask in 2.93 of 9.89 interrupts. Each costs a hardirq and a poll run again (`NAPI_STATE_MISSED`).
- **This is r105's "second interrupt within a microsecond of the poll's start":** the ring's own interrupt, sent before its mask, and not another device's. r105's `irq_to_poll` tail of 2–64 µs was its own artefact: a lost interrupt stamped the time, and the poll run again for it was then counted from that stamp across the whole of the poll before (polls with work take a mean of 7.9 µs, a third of them 8 µs or more). r106 stamps only interrupts that schedule a poll: 74% of polls start 128–255 ns after their interrupt, 22% 512–1023 ns, and none wait a microsecond.
- **What they cost, as far as is timed here: about 0.85 µs a WRITE, 1.7k ring cycles (1.2%).** An empty poll after an interrupt takes 379 ns from the handler to its return (143 ns of it the poll), × 1.82. An empty poll run again takes 100 ns, × 1.56. Not timed: the exception entry, the GIC's acknowledgment and the exit for the 4.75 interrupts a WRITE that bring no work (2.93 lost, 1.82 stale), and `net_rx_action()`'s own overhead for the empty polls. At a few hundred nanoseconds an interrupt that would be another 1–3k cycles, so all told perhaps 2–3% of the rings' cycles.

**What would remove them:**

- **In the card: interrupt moderation** (a minimum gap or a completion count before the next interrupt). That would take most of the lost interrupts, and with them the polls run again. It is Xsight's, and is worth asking for.
- **In the kernel: NAPI's deferral of interrupts** (`napi_defer_hard_irqs` with `gro_flush_timeout`, both 0 at the settled setting). It keeps the ring masked and polls again from a timer for a set number of empty polls, so fewer interrupts and stale ones. It costs latency to the frames, and the rings' CPU time goes from interrupts to the timer. An A/B of a few settings would show what it saves; it is not run here.

Neither is large: at most 2–3% of the rings' cycles, which are themselves about a third of nfsd's.

## Round 42, closed

Where the rings' stalls are, as far as this round could place them (runs 100 to 106):

- **Counted (r100, r101):** 19% of the rings' cycles are memory stalls, from about 1.06k DRAM reads and 1.2k L3 hits a WRITE. 25% are backend stalls not on memory.
- **The payload goes through DRAM (r101).** The card's writes are filled into the L3 and evicted before the drives read them, at about 1.8 DRAM requests a payload line. The region is larger than the L3, by the footprint rule as it stands. Nothing is stashed.
- **The poll's own first touches are not where the DRAM reads are (r102 to r105).** The completions, ring entries, frames' `struct page`s, the doorbell and the re-arm are at the floor or near it. The header lines come from the L3, and xeu's next-frame prefetch hides them (`rx_prefetch` keeps its place). The refill's `struct page`s come from the L3, and the core already overlaps them: prefetching them gained nothing (r103, not kept). The head's completion costs 0.4k cycles a WRITE.
- **The interrupts (r105, r106):** 9.89 a WRITE, of which 2.93 come too late to be needed and 1.82 are stale. Perhaps 2–3% of the rings' cycles, for which the remedies are the card's interrupt moderation or NAPI's deferral.

**Still not placed:** the bulk of the rings' 1.06k DRAM reads a WRITE. They are not in the driver's poll, so they are in skb and GRO, TCP and the transmit side. Naming them needs a precise profile (SPE, which this firmware does not offer, or pseudo-NMI sampling after a reboot). **Nothing from this round is kept in xeu.**
