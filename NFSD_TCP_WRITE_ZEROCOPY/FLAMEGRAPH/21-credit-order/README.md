# Round 21: the region credit lists ordered from their newest end

**What was profiled:** fl40-loan64, build 499 at the settled setting with xeu `irq_msix_mask` Y (round 20's B). nfsd cost 112.4 µs a WRITE; the loan path's own `svc_tcp_rx_*` frames about 15 of it.
- `svc_tcp_rx_credit_admit_to` was 2.15 µs a WRITE self, 2.60 inclusive. 91% of its samples were one loop ([annotate-admit-fl40.txt](annotate-admit-fl40.txt)): `svc_tcp_rx_credit_link_locked()` walking the budget's in-flight records from the oldest to find where the admitted record goes, under the budget's lock. Each step loads the next record's ticket, a cache line written on another CPU.
- A record is admitted only as the oldest waiter or with nobody waiting, so its ticket is nearly always the newest, and the walk went past all of the about 64 records in flight to append at the tail.

**The change:** "SUNRPC: Order region credit lists from their newest end" (`c1b4326c9b99`). The in-flight records and the waiting transports are both walked from their newest entry; the order the lists keep is the same.

**Runs 2026-10-25 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run41.sh`, the runs `/var/tmp/zc-run-logs/t2-r41/`.
- A is build 499's own sunrpc (`EFD53C53F61C1F0CDF37D89`), B the tip with the change (`1D9C18007E10E362F8EFD03`, built by `build-sunrpc.sh`). Before every run sunrpc was swapped (`swap-sunrpc.sh`) and the host settled.
- KUnit on B: 7, 10, 52 and 7 pass (nfsd-receive-bvec, nfsd4-receive-bvec, sunrpc-svcsock-rx, sunrpc-xdr-bvec).
- The first attempt stopped at its first swap: systemd-hostnamed's private mount namespace still held `/proc/fs/nfsd`, so nfsd and sunrpc would not unload. `swap-sunrpc.sh` now stops that service (and timedated and localed), which come back on demand, and names any other process that holds the mounts.

## Outcome: the walk is gone, nfsd is not measurably cheaper

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | region peak in use, pages |
|---|---|---|---|---|---|
| r41-1-loan64-A | 499's own | 40.54 | 48.5 | 110.1 | 6645 |
| r41-2-loan64-B | the change | 40.51 | 50.8 | 110.9 | 6577 |
| r41-3-loan64-A | 499's own | 40.53 | 48.8 | 110.2 | 6485 |
| r41-4-loan64-B | the change | 40.53 | 48.5 | 106.9 | 6484 |
| r41-5-loan64-A | 499's own | 40.53 | 48.7 | 109.6 | 6486 |
| r41-6-loan64-B | the change | 40.47 | 49.5 | 110.7 | 6510 |
| r41-7-loan64-A | 499's own | 40.54 | 48.4 | 108.8 | 6560 |
| r41-8-loan64-B | the change | 40.45 | 47.5 | 109.7 | 6492 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)).

| | rings µs a WRITE | nfsd µs a WRITE |
|---|---|---|
| A, 4 runs | 48.60 ± 0.16 | 109.67 ± 0.55 |
| B, 4 runs | 49.08 ± 1.22 | 109.55 ± 1.60 |
| B - A | +0.48 ± 0.62 | **-0.13 ± 0.85** |

- **nfsd:** no change the A/B can resolve. Three B runs are where the A runs are; one (r41-4) is 3 µs below.
- **The profile** (fl41-loan64 against fl40-loan64, [top-nfsd.txt](top-nfsd.txt)): `svc_tcp_rx_credit_admit_to` went from 2.60 µs a WRITE inclusive to 0.36, but nfsd's busy time only from 112.4 to 112.2. Small rises in many frames take up most of the rest, none of them above 0.5.
- So the misses the walk took were not saved, only moved: the records' lines are written by the threads that own them, and whoever reads them next pays. The walk was where that cost showed, not what caused it.
- **Not changed:** the rings, the rate, the region's peak pages, refused loans (1.61 to 1.63 million a run on both sides), the hand-offs.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid for this round: `twoport.sh` opens the port after the driver's first snapshot, and without an open before each run (round 20 had one) the counts went back to 0 inside the window. Round 20's figures are sound: its first snapshots read about 250.

## Latency

[latency.txt](latency.txt), untraced runs:

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, the change | 0.428 ms | 0.498 | 0.578 | 0.438 |
| A1, 499's own | 0.440 | 0.510 | 0.578 | 0.452 |
| B2, the change | 0.424 | 0.494 | 0.561 | 0.434 |
| A2, 499's own | 0.444 | 0.518 | 0.586 | 0.455 |

- B's p50 is 0.01 to 0.02 ms below A's in both pairs; runs differ by that much, so it is no finding.

## Decision

- **Kept (Mike, 26 October).** It saves nothing measurable at 64 WRITEs in flight, but it takes an O(n) walk out from under the budget's lock, which would grow with a larger budget. Its message no longer claims a saving of about 2 µs a WRITE; it says the walk is gone.
