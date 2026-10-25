# Round 19: rounds 04 and 12 together. The reply cache kept, the deeper stack not

**The change:** the two settings that saved a loaned WRITE when run apart on build 499 (25 October), run together:
- `page_pool.rx_region_stack` 1024 against the ring's 256 ([round 04](../04-region-stack/README.md)): the rings -3.5 us a WRITE, nfsd -4.0, about 890 more region pages in use at peak.
- `nfsd.drc_max_entries` 4096 against the 262144 nfsd sizes itself to ([round 12](../12-drc-size/README.md)): nfsd -3.4.

**Runs 2026-10-25 on tardis2**, build 499 (the tip, `bca1a2f143eb`), on the kernel's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run39.sh`, the runs `/var/tmp/zc-run-logs/t2-r39/`.
- B was the settled setting with both (`a08f7b5d0ea6`). A held both at 0 through `EXP_RX_REGION_STACK` and `EXP_DRC_MAX_ENTRIES`, which `settled.sh` audits as off the settled setting in those two alone.
- Before every run, on both sides, `settled.sh apply` restarted nfs-server and opened port a again, so no side had a fresh pool or cache the other lacked.

## Outcome: the rings -3.15 us a WRITE, nfsd -3.95; the region 940 pages larger at peak

| run | stack | entries | GB/s | ring µs a WRITE | nfsd µs a WRITE | enqueued a WRITE | woken a WRITE | pages to the ring-full path a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|---|---|---|
| r39-1-loan64-A | 0 | 262144 | 40.48 | 60.9 | 112.2 | 5.60 | 4.44 | 0.70 | 6250 |
| r39-2-loan64-B | 1024 | 4096 | 40.48 | 59.1 | 106.1 | 5.47 | 4.38 | 0.00 | 7302 |
| r39-3-loan64-A | 0 | 262144 | 40.48 | 62.0 | 111.4 | 5.46 | 4.36 | 0.70 | 6508 |
| r39-4-loan64-B | 1024 | 4096 | 40.42 | 59.3 | 110.3 | 5.52 | 4.41 | 0.00 | 7209 |
| r39-5-loan64-A | 0 | 262144 | 40.35 | 61.0 | 111.3 | 5.42 | 4.33 | 0.71 | 6521 |
| r39-6-loan64-B | 1024 | 4096 | 40.53 | 56.9 | 106.7 | 5.39 | 4.32 | 0.00 | 7492 |
| r39-7-loan64-A | 0 | 262144 | 40.40 | 60.7 | 110.1 | 5.51 | 4.40 | 0.71 | 6355 |
| r39-8-loan64-B | 1024 | 4096 | 40.48 | 56.7 | 106.1 | 5.47 | 4.37 | 0.00 | 7403 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)).

| | rings µs a WRITE | nfsd µs a WRITE | region peak, pages |
|---|---|---|---|
| A, 4 runs | 61.15 ± 0.58 | 111.25 ± 0.87 | about 6410 |
| B, 4 runs | 58.00 ± 1.39 | 107.30 ± 2.02 | about 7350 |
| B - A | **-3.15 ± 0.75** | **-3.95 ± 1.10** | **+940 (15 MiB)** |

- **The rings:** what round 04 saved alone. Every B run is below every A run.
- **nfsd:** about what either round saved alone, not the two together (-7.4). Three B runs are at 106.1 to 106.7, one at 110.3. The two changes overlap on nfsd, or one of them is smaller than its own A/B said; this A/B does not tell which.
- **Not changed:** the rate (40.35 to 40.53 GB/s), the hand-offs (5.4 to 5.6 enqueues and 4.3 to 4.4 wakes a WRITE on both sides), refused loans.
- **The profile** of B (fl39-loan64, [top-rings.txt](top-rings.txt), [top-nfsd.txt](top-nfsd.txt)): the rings 56.60 us a WRITE, nfsd 108.93, against fl15's 59.21 and 111.82 at both 0. `nfsd_cache_lookup` 3.18 to 2.77, `page_pool_alloc_frag` on the rings 5.07 to 4.41.

## Latency

[latency.txt](latency.txt), untraced runs:

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, both | 0.432 ms | 0.502 | 0.578 | 0.441 |
| A1, neither | 0.424 | 0.506 | 0.569 | 0.438 |
| B2, both | 0.424 | 0.498 | 0.578 | 0.436 |
| A2, neither | 0.424 | 0.498 | 0.578 | 0.435 |

- No difference.

## Decision (Mike, 25 October)

- **`drc_max_entries` 4096 is kept** in `settled.sh`. It takes no region pages.
- **`rx_region_stack` 1024 is not kept**: "we cannot afford to increase the region ... we need to avoid blowing out the L3 cache". `settled.sh` holds it at 0, the ring's 256, and its saving on the rings (about 3 us a WRITE) is given up.
- From this the project has a rule, in AGENTS.md and this directory's README: a change that raises the region's peak pages in use is not kept, whatever it saves.
- The deeper stack's saving does not come from the overflow it was for (round 04), so a way to have it without more pages in use is not known. Finding where it comes from could give one.
