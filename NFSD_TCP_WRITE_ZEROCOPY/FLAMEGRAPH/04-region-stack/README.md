# Round 04: a deeper stack for the region's page pools

**The change:** "page_pool: Let a pool bound to the receive region keep more pages" (`dc23eac70f34`).
- A pool bound to the fixed receive region keeps the pages it gets back on a stack of the ring's size, 256.
- Round 01's profile showed the stack overflowing: about 7% of pages went back to the region, an estimated 5 µs a WRITE.
- `page_pool.rx_region_stack` sets the stack's depth; 0, the default, keeps the ring's size. It is read when a pool is made, so each side of the A/B opens the port again.

**Runs 2026-10-25 on tardis2**, build 499 (the tip, `bca1a2f143eb`), on the kernel's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`), the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run33.sh`, the runs `/var/tmp/zc-run-logs/t2-r33/`.
- A is `rx_region_stack` 0 (the ring's 256), B 1024. Before each run the port was opened again (down and up) and settled.
- KUnit on build 499: 7, 10, 52 and 7 pass.

## Outcome: the rings 3.5 µs a WRITE cheaper (-6%), nfsd 4.0; the region holds 890 more pages

| run | stack | GB/s | ring µs a WRITE | nfsd µs a WRITE | pages to the ring-full path a WRITE | slow allocations a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|---|
| r4-1-loan64-A | 0 | 40.43 | 61.5 | 111.0 | 0.7 | 0.3 | 6497 |
| r4-2-loan64-B | 1024 | 40.53 | 56.6 | 108.0 | 0.0 | 0.0 | 7327 |
| r4-3-loan64-A | 0 | 40.53 | 58.8 | 110.5 | 0.7 | 0.3 | 6454 |
| r4-4-loan64-B | 1024 | 40.52 | 56.2 | 105.7 | 0.0 | 0.0 | 7464 |
| r4-5-loan64-A | 0 | 40.48 | 58.8 | 113.1 | 0.7 | 0.3 | 6425 |
| r4-6-loan64-B | 1024 | 40.41 | 56.6 | 108.8 | 0.0 | 0.0 | 7253 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt); the page_pool counters are the port's, read around each run).

- **The rings:** 59.7 to 56.5 µs a WRITE against the three A runs. Round 12's six runs, all at stack 0, cost the rings 59.3 to 61.4; against all nine stack-0 runs (60.0 ± 1.0) the change is **-3.5 ± 0.4 µs a WRITE**. Every B run is below every stack-0 run.
- **nfsd:** 111.5 to 107.5 µs a WRITE, **-4.0 ± 1.2**, from the A runs alone (round 12's runs vary the reply cache, which moves nfsd).
- **The region:** about 6460 pages in use at peak at stack 0, 7350 at 1024: **+890 pages, 14 MiB, +14%**, 22% of the region's 32768. The ring still reuses a pool's pages last in, first out, so the pages it takes are the hottest, but its working set is 14 MiB larger, against keeping the L3 hot.
- **Not changed:** the rate (40.4 to 40.5 GB/s), refused loans (1.54 to 1.62 million a run on both sides), latency (below).

## The overflow is not where the saving comes from

- **At stack 0 the stack overflows only in the first run after the port opens.** In each A run about 1 page in 64 a WRITE went to the ring-full path and 0.3 were allocated slowly.
- Round 12's runs were made at stack 0 without opening the port again. After its first run (r12-1, 0.7 and 0.3 again) neither counter moved: 7580 pages to the ring-full path over the 2.15 million WRITEs of r12-2, and nothing after.
- **Those runs cost the rings 59.3 to 61.4 µs a WRITE, as much as the A runs that overflowed.** So the overflow itself costs nothing measurable. The deeper stack saves something else, not found: the profile's one difference is `page_pool_alloc_frag` on the rings, 5.07 µs a WRITE at stack 0 against 4.49 at 1024 (fl15 and fl4, [top-rings.txt](top-rings.txt)).
- Every B run was a first run after an open too. B at steady state was not run; it has no overflow either way.

## The profile

fl4-loan64 at stack 1024, against fl15-loan64 at stack 0 (round 15's profile, the same kernel), µs a WRITE:

| | fl15 (0) | fl4 (1024) |
|---|---|---|
| the rings, busy | 59.21 | 58.80 |
| nfsd, busy | 111.82 | 110.95 |
| `page_pool_alloc_frag` on the rings | 5.07 | 4.49 |

- One profile a side resolves 0.4 µs a WRITE on the rings, not the A/B's 3.5. A single run at stack 0 lies anywhere from 58.8 to 61.5, and perf at 999 Hz costs the rings some of their rate, so the A/B is the measurement.

## Latency

[latency.txt](latency.txt), untraced runs:

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, 1024 | 0.461 ms | 0.522 | 0.578 | 0.456 |
| A1, 0 | 0.465 | 0.528 | 0.610 | 0.464 |
| B2, 1024 | 0.489 | 0.561 | 0.668 | 0.488 |
| A2, 0 | 0.420 | 0.494 | 0.553 | 0.431 |

- No difference: B averages p50 0.475, A 0.443, and each side's runs differ by as much. B1 had one completion of 10.5 ms.

## Decision: not kept (Mike, 25 October)

- Run with round 12 in [round 19](../19-stack-and-drc/README.md), the deeper stack again saved the rings about 3 µs a WRITE, and again held about 940 more of the region's pages at peak.
- Mike: "we cannot afford to increase the region ... we need to avoid blowing out the L3 cache". `settled.sh` holds `rx_region_stack` at 0, the ring's 256.
- A depth between 256 and 1024 would hold fewer pages but not none, so it falls under the same rule. Since the overflow is not the cause, where the saving comes from is still to be found; that could give it without more pages.
