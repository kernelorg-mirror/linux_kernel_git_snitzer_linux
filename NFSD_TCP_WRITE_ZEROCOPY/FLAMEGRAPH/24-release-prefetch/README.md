# Round 24: a loaned record's struct pages prefetched a batch ahead of release

**What was profiled:** fl44-loan64, round 23's B at the settled setting. nfsd cost 106.4 µs a WRITE.
- Round 23 took the release's atomics away, but the miss on each page's `struct page` line moved with them: `svc_tcp_rx_release_refs`' self time went from 0.92 to 4.37 µs a WRITE, where the inlined `page_pool_unref_netmem()` reads the count.
- 4.37 µs for about 64 pages a WRITE is about 68 ns a page, a full miss for each: they are taken one after another, not overlapped.
- Round 16 prefetched every page of a record in one pass before the release and saved nothing in the A/B, with each drop behind it then an ordered atomic. Now the drops are mostly plain loads.

## The change

- **"SUNRPC: Prefetch a loaned record's struct pages a batch ahead of release"** (`57f883cc3294`): `svc_tcp_rx_release_refs()` `prefetchw()`s the struct page of the first batch of entries (`SVC_TCP_RX_PUT_BATCH`, 16), then, at each entry, the one a batch ahead, so that about eight pages' misses are in flight while the drops go on. For writing, as a run held whole sets its page's count to 1.
- No page is held longer or returned later, and no line is touched that the release does not read anyway.

**Runs 2026-10-26 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run46.sh`, the runs `/var/tmp/zc-run-logs/t2-r45/`.
- A is the tip before the change (`09926B26CE0CD23D4F1B72F`, round 23's B), B the change (`35170F2B88E9FBF01D459D9`, built by `build-sunrpc.sh`, all 327 exports at their versions). Before every run sunrpc was swapped (`swap-sunrpc.sh`) and the host settled; every load checked dmesg for a new WARNING, and there was none.
- KUnit on B: 7, 52, 7 and 10 pass.

## Outcome: kept. The release 2.8 µs cheaper; nfsd 3.2 µs a WRITE, some of it A's drift

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | region peak in use, pages |
|---|---|---|---|---|---|
| r46-1-loan64-A | before | 40.49 | 51.1 | 107.0 | 6394 |
| r46-2-loan64-B | prefetch | 40.55 | 51.0 | 101.3 | 6679 |
| r46-3-loan64-A | before | 40.47 | 49.3 | 106.0 | 6463 |
| r46-4-loan64-B | prefetch | 40.53 | 49.4 | 101.4 | 6454 |
| r46-5-loan64-A | before | 40.48 | 48.8 | 104.5 | 6483 |
| r46-6-loan64-B | prefetch | 40.54 | 49.6 | 101.9 | 6518 |
| r46-7-loan64-A | before | 40.59 | 49.1 | 101.8 | 6572 |
| r46-8-loan64-B | prefetch | 40.56 | 50.7 | 101.8 | 6570 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)). No run stalled: the slowest WRITE of each took 110 to 132 ms.

| | rings µs a WRITE | nfsd µs a WRITE |
|---|---|---|
| A, 4 runs | 49.58 ± 1.04 | 104.83 ± 2.26 |
| B, 4 runs | 50.17 ± 0.79 | 101.60 ± 0.29 |
| B - A | +0.60 ± 0.65 | **-3.23 ± 1.14 (-3%)** |

- **The profile** (fl46-loan64 against fl44-loan64, [top-nfsd.txt](top-nfsd.txt)): `svc_tcp_rx_release_refs`' self time 4.37 to 1.75 µs a WRITE, the release inclusive 9.28 to 6.48: 2.8 µs. The bulk put and the ring's recycling are the same (3.81 and 3.61 inclusive). The profile measures the frame itself, so the drift below does not touch it.
- **The A/B agrees in direction and roughly in size,** about 2.8 standard errors. B's four runs are tight (101.3 to 101.9). A fell run by run, 107.0, 106.0, 104.5, 101.8, and its last run is level with B's: some of the 3.2 is A's drift, and the profile's 2.8 is the firmer figure.
- **The calm runs:** the cheapest runs on either side had the fewest enqueues, wakes and switches a WRITE (r46-7, A: 5.30, 4.29 and 5.78; B's runs 5.33 to 5.37, 4.30 to 4.33, 5.79 to 5.81; A's first three 5.39 to 5.45, 4.34 to 4.40, 5.83 to 5.88). About 0.1 a WRITE fewer of each, which at about 1 µs a wake is a fraction of a µs by itself.
- **The two profiles' nfsd totals are about the same** (106.44 and 106.36), as round 16's were. One profiled run each, with about 2 µs between runs, so it cannot overrule the frame's drop or the A/B.
- **The region:** B's peaks 6454 to 6679, A's 6394 to 6572; three of B's four are inside A's range. The change holds no page longer, and fetches only the struct page lines the release reads anyway, about 64 a WRITE.
- **Not changed:** the rings, the rate, refused loans, hand-offs.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

**Rounds 23 and 24 together:** the release 10.14 to 6.48 µs a WRITE inclusive (fl42 to fl46), nfsd -1.33 and -3.23 in their A/Bs.

## Latency

[latency.txt](latency.txt):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, prefetch | 408 µs | 477 | 553 | 419 |
| A1, before | 408 | 489 | 594 | 422 |
| B2, prefetch | 428 | 498 | 569 | 435 |
| A2, before | 436 | 514 | 586 | 449 |

- The same: the pairs agree, B2 and A2 both a little slower than B1 and A1.

## Decision

- **Kept (Mike, 26 October).** It is code with no parameter, so `settled.sh` has nothing to hold.
