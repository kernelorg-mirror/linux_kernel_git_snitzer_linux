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
