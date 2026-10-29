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

## 1. The stalls counted (r100, to run)

[run100.sh](run100.sh): one loan64 run at the current state, no module swapped. From 10 s into the timed load, five 3 s `perf stat -C 0-7` windows, one counter group each (1 s apart), the WRITEs in each window counted from `/proc/net/rpc/nfsd`; then 3 s of `bpftrace` on `napi:napi_poll` for each ring poll's frames.
