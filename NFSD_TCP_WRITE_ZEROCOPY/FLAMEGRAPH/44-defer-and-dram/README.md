# Round 44: NAPI's deferral of interrupts, and where the rings read from DRAM

**The ask (Mike, 29 October):** after round 43, the `napi_defer_hard_irqs` A/B as round 44, and with it the chase for where the rings' ~1.06k DRAM reads a WRITE come from.

**What there is from before:**

- **The interrupts (round 42's r106).** The card has no interrupt coalescing, and raises 9.89 receive interrupts a WRITE. 5.14 of them start a poll with work. 1.82 are stale: pending while the ring was masked, they arrive 0.8 µs after the unmask for work already taken. 2.93 arrive while NAPI already has the ring, so the poll runs again. Timed, the empty polls cost about 1.7k ring cycles a WRITE; with the interrupts' entries, perhaps 2–3% of the rings' cycles.
- **The DRAM reads (round 42).** r100 and r101 counted about 1.06k `ll_cache_miss_rd` and 2.3k lines from beyond the L2 a WRITE, on CPUs 0–7 together. Rounds 42 (r102 to r105) and 41 (r97) timed the lines the poll and GRO read first, and none of them come from DRAM in any number.
- **The profile (round 37's r80).** It put 40% of the rings' L3-miss samples in GRO's frames. Round 41 showed GRO's own loads hit, so those samples are likeliest xeu's prefetch of the next frame, outstanding while GRO runs. Nothing has yet split the rings' misses between the RX CPUs (0, 2, 4, 6) and the TX CPUs (1, 3, 5, 7).

## 1. NAPI's deferral of the receive interrupts (r107, to run)

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

## 2. Where the rings read from DRAM (r108, to run)

[run108.sh](run108.sh): the kept xeu at the settled setting, one loan64 run. From 10 s into its timed load:

1. **Per CPU on 0–7, counted:** two 3 s windows, of `ll_cache_miss_rd`, `l2d_cache_refill`, `l2d_cache_lmiss_rd`, `l1d_cache_refill`, then `stall_backend_mem`, `bus_access_rd`, `mem_access_rd`, `l3d_cache_refill`. These are summed for the RX CPUs and the TX CPUs apart, a WRITE. Whether the DRAM reads are the receive side's or the transmit side's: the TX CPUs complete the ACKs and the NFS replies, whose skbs were built elsewhere.
2. **Sampled:** 5 s of `ll_cache_miss_rd`, one in 5000, on CPUs 0–7 with call chains, by symbol for each group.
3. **The same with `rx_prefetch` N,** so that no prefetch of the next frame is outstanding while GRO runs. The samples then land nearer the loads that miss, and the remainder is not xeu's prefetch. `settled.sh` puts `rx_prefetch` back.

A sample still lands on the instruction running when the counter overflows, and the core's own prefetcher's misses are counted too, so a symbol's share is where misses are outstanding, not proof of whose load they are. What the split and the two profiles name is then for a diagnostic of the round 41/42 kind, timing those lines at their first touch.
