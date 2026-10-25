# Round 12: the size of the duplicate reply cache

**The change:** "NFSD: Let the size of the duplicate reply cache be set" (`635e44bf37bf`).
- On tardis2 the NFSv3 duplicate reply cache sizes itself to 262144 entries (63 MB). In round 10b's profile it was full, with 0 hits, and cost about 4.5 µs a WRITE of cold misses.
- `nfsd.drc_max_entries` sets the size; 0, the default, keeps the computed one. It is read when nfsd starts, so each side of the A/B restarts nfs-server.
- v3 stays: flexfiles data servers use it.

**Runs 2026-10-25 on tardis2**, build 499 (the tip, `bca1a2f143eb`), on the kernel's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`), the settled setting, `rx_region_stack` 0. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run33.sh`, the runs `/var/tmp/zc-run-logs/t2-r33/`.
- Three sizes: 0 (262144 entries in 32768 buckets), 32768 (4096 buckets) and 4096 (512 buckets), each run after nfs-server was restarted and settled.

## Outcome: 4096 entries saves nfsd 3.4 µs a WRITE (-3%); 32768 saves nothing

| run | entries | GB/s | ring µs a WRITE | nfsd µs a WRITE | longest chain | hits |
|---|---|---|---|---|---|---|
| r12-1-loan64-drc0 | 262144 | 40.57 | 59.3 | 112.7 | 6 | 0 |
| r12-2-loan64-drc32768 | 32768 | 40.47 | 60.1 | 113.6 | 7 | 0 |
| r12-3-loan64-drc4096 | 4096 | 40.48 | 60.3 | 108.8 | 8 | 0 |
| r12-4-loan64-drc0 | 262144 | 40.48 | 60.6 | 110.5 | 8 | 0 |
| r12-5-loan64-drc32768 | 32768 | 40.46 | 59.3 | 109.3 | 8 | 0 |
| r12-6-loan64-drc4096 | 4096 | 40.42 | 61.4 | 107.4 | 8 | 0 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt); the longest chain is cumulative since boot).

- **nfsd:** 111.6 µs a WRITE at 262144, 111.45 at 32768, 108.1 at 4096. 4096 against the other four runs: **-3.4 ± 1.2 µs a WRITE**, and both 4096 runs are below all four. Round 04's three A runs, at the default size, cost 110.5 to 113.1, the same.
- **32768 entries is no cheaper than 262144.** Only a cache small enough to stay hot helps.
- **Not changed:** the rings (59.3 to 61.4, the cache is on nfsd), the rate (40.4 to 40.6 GB/s), latency (below).
- Two runs at each size, one fewer than the method's three. The two 4096 runs agree within 1.4.

## The profile

fl12-loan64 at 4096, against fl15-loan64 at 262144 (round 15's profile, the same kernel), µs a WRITE:

| | fl15 (262144) | fl12 (4096) |
|---|---|---|
| nfsd, busy | 111.82 | 110.15 |
| every stack through `nfsd_cache_*` | 3.62 | 3.29 |
| `nfsd_cache_lookup` | 3.18 | 2.93 |
| `nfsd_cache_insert` | 0.97 | 0.84 |

- The cache's own frames are only 0.3 µs a WRITE cheaper in the profile. The rest of the A/B's 3.4 is not in them: the misses of a cold cache may land in what runs after it, or one profile a side is too few. Not resolved.
- fl12's rings, 63.3 µs a WRITE, are above every untraced run; the profile costs the rings part of their rate, and the cache does not run there.

## Latency

[latency.txt](latency.txt), untraced runs:

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, 4096 | 0.436 ms | 0.506 | 0.569 | 0.443 |
| A1, 262144 | 0.465 | 0.518 | 0.569 | 0.456 |
| B2, 4096 | 0.424 | 0.498 | 0.561 | 0.434 |
| A2, 262144 | 0.420 | 0.498 | 0.586 | 0.434 |

- No difference: each WRITE misses the cache once at either size.

## For Mike

- Keeping it is `nfsd.drc_max_entries` 4096 in `settled.sh`: 3.4 µs a WRITE off nfsd, with nothing else moved.
- The cache never hits on this rig: the client retransmits nothing. Whether 4096 entries covers what flexfiles data servers retransmit is not tested here.
