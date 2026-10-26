# Round 23: a page's neighbouring loaned buffers returned in one drop

**What was profiled:** fl42-loan64, round 22's B at the settled setting. nfsd cost 106.2 µs a WRITE.
- The release was the largest of nfsd's loan frames: `svc_tcp_rx_release_refs` 10.1 µs a WRITE inclusive, 5.7 of it self in `page_pool_put_netmem_bulk`, most of that the `ldaddal` on each page's `pp_ref_count` (round 22's annotate).
- At release a page's count was still 2 in 85% of drops (round 22's [refs.txt](../22-ref-takeover/refs.txt)). Round 22 took that to mean the page's other buffer was on loan elsewhere; refs.bt read every count of a batch before the batch dropped any, so it could not tell.

**Where the other buffer was** ([pairs.txt](pairs.txt), `pairs.bt`, the first look on build 499's own sunrpc, whose release and entry order round 22 does not change):
- Of 49.4 M entries handed to the bulk put in 10 s, 18.3 M (37%) were on the page of the entry just before them, and 99% of those at a count of 2: the record held both of the page's references, side by side. xeu cuts a 16 KiB page into two 8 KiB buffers and fills them in turn, so a flow's consecutive frames often take one page's two halves.
- Dropped one at a time, the first of such a pair takes the atomic (2 to 1) and the second the read-only path (1). About 18 M of the release's roughly 25 M atomics in the 10 s were the first of a pair.
- The rest: 6.2 M entries already at 1, about 6 M at 2 with the page's other buffer in another record or batch.

## The change

- **"SUNRPC: Return a page's neighbouring loaned buffers in one drop"** (`3e95efcd28c0`): `svc_tcp_rx_release_refs()` counts a run of entries on one page and drops its references together with `page_pool_unref_netmem()`. When the record holds every reference to the page, that is a read of its count, which `page_pool_unref_netmem()` then sets to 1, and no atomic; the page goes in the batch at 1, and the bulk put returns it on its own read-only path. When another holder remains, the run costs one atomic instead of one each.
- No page is held longer or returned later.

**Runs 2026-10-26 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run45.sh`, the runs `/var/tmp/zc-run-logs/t2-r44/`.
- A is the tip before the change (`4BC3A057D06EC2EA1655F73`, round 22's B), B the change (`09926B26CE0CD23D4F1B72F`, built by `build-sunrpc.sh`, all 327 exports at their versions). Before every run sunrpc was swapped (`swap-sunrpc.sh`) and the host settled; every load checked dmesg for a new WARNING, and there was none.
- KUnit on B: 7, 52, 7 and 10 pass.

## Outcome: kept. The atomics went, the miss moved; nfsd about 1 µs cheaper a WRITE

**The pairs go** ([pairs.txt](pairs.txt), 10 s of a run on each side):

| | pages handed to the bulk put | of them the page before's | at a count of 2 or more (an atomic) |
|---|---|---|---|
| A | 48.9 M | 18.3 M | 24.5 M |
| B | 29.1 M | 0 | 4.9 M |

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | region peak in use, pages |
|---|---|---|---|---|---|
| r44-1-loan64-A | before | 40.52 | 47.5 | 107.8 | 6444 |
| r44-2-loan64-B | pairs | 40.50 | 51.4 | 104.6 | 6500 |
| r44-3-loan64-A | before | 40.48 | 49.7 | 104.3 | 6516 |
| r44-4-loan64-B | pairs | 40.50 | 50.8 | 105.5 | 6511 |
| r44-5-loan64-A | before | 40.57 | 50.9 | 105.2 | 6438 |
| r44-6-loan64-B | pairs | 40.50 | 51.6 | 105.1 | 6666 |
| r44-7-loan64-A | before | 40.55 | 50.9 | 107.0 | 6395 |
| r44-8-loan64-B | pairs | 40.44 | 49.6 | 103.8 | 6451 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)). No run stalled: the slowest WRITE of each took 111 to 127 ms.

| | rings µs a WRITE | nfsd µs a WRITE |
|---|---|---|
| A, 4 runs | 49.75 ± 1.60 | 106.08 ± 1.61 |
| B, 4 runs | 50.85 ± 0.90 | 104.75 ± 0.73 |
| B - A | +1.10 ± 0.92 | **-1.33 ± 0.88** |

- **nfsd:** about 1.5 standard errors; every B run (103.8 to 105.5) is inside A's range (104.3 to 107.8).
- **The profile** (fl44-loan64 against fl42-loan64, [top-nfsd.txt](top-nfsd.txt)): `page_pool_put_netmem_bulk`'s self time 5.70 to 1.04 µs a WRITE, the atomic gone; but `svc_tcp_rx_release_refs`'s self 0.92 to 4.37, where the inlined `page_pool_unref_netmem()` now reads each page's count. The release's inclusive time is 10.14 to 9.28, about 0.9 µs, as the A/B has it.
- So the cost was the `struct page` line coming from another CPU, not the atomic as such: a plain load misses on it as the atomic did. The release's misses are now loads with no ordering between them, which a prefetch issued ahead of them can overlap; round 16's prefetch before the release saved nothing when each drop was an ordered atomic. That is round 24.
- **The rings:** +1.10 ± 0.92, not resolved; the change does not run on the ring CPUs, and A's last two runs (50.9) are where B's are.
- **The region:** B's peaks 6451 to 6666, A's 6395 to 6516. Inside what runs differ by (round 22's A was 6388 to 6540): the change holds no page longer.
- **Not changed:** the rate, refused loans, hand-offs and wakes a WRITE.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## Latency

[latency.txt](latency.txt):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, pairs | 424 µs | 502 | 618 | 436 |
| A1, before | 420 | 494 | 545 | 431 |
| B2, pairs | 408 | 477 | 545 | 419 |
| A2, before | 416 | 477 | 545 | 425 |

- The same. B1's p99 of 618 did not come back in B2, nor in B's traced runs (553, 537).

## Decision

- **Kept (Mike, 26 October)**, as the base for round 24's prefetch. It is code with no parameter, so `settled.sh` has nothing to hold.
