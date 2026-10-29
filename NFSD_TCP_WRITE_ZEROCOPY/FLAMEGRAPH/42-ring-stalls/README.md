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

## 2. What the rings read beyond L2, and where the card's writes go (r101, to run)

SPE was the plan, and is not to be had: `arm_spe_pmu` loads, but the firmware's ACPI MADT gives no CPU an SPE overflow interrupt (0 on all 64; the PMU's is 23), so the driver creates no device. Making one would take a patched MADT through the initrd (`CONFIG_ACPI_TABLE_UPGRADE=y`), a reboot, and SPE enabled for the kernel by the firmware, which an empty field suggests it is not. The module was unloaded again. In its place (Mike, 29 October), two looks with no change to the host:

1. **The switches within one run,** on the ring CPUs: `rx_prefetch` and `rx_gro_frags` in turn (YY NY YN YY NY YN, 3 s each), counting cycles, instructions, `bus_access_rd`, `l2d_cache_refill`, `ll_cache_miss_rd` and `stall_backend_mem`. Without `rx_prefetch`, the lines the core's prefetcher brings in after a frame's headers should move with it if they are payload; without `rx_gro_frags`, the built skbs' lines come back.
2. **The mesh** (`arm_cmn_0`, the CMN-700's counters, system-wide), at rest and under load, four groups of four in turn:
   - what the PCIe devices write and read: `rnid_s0/s1/s2_wdata_beats` (the card's received frames and completions, mostly) and `rnid_s0/s1/s2_rdata_beats` (the drives reading the payload, the card sending);
   - the L3: `hnf_slc_sf_cache_access`, `hnf_cache_miss`, `hnf_cache_fill`, `hnf_slc_eviction`;
   - DRAM and stashing: `hnf_mc_reqs`, `hnf_stash_data_pull`;
   - snoops: `hnf_snp_sent`, `hnf_sf_hit`, `hnf_sf_evictions`, `hnf_dir_snoops_sent`.

   Whether the card's writes are filled into the L3 (fills against the device's write beats), how much the payload takes of it, and whether anything is stashed. If the card's writes go to DRAM, every completion and header line the rings read costs a DRAM read, and the lever is outside the CPU: the card stashing those lines (CMN supports stashing; whether the E1 can ask for it is Xsight's).

[run101.sh](run101.sh): `busread` on CPU 63 again; the mesh's groups at rest during the rest before the load; one loan64 run with the switch windows from 10 s into its timed load and the mesh's groups under load after them. `settled.sh` puts both switches back.
