# Round 09: BIG TCP on the server

**The change:** the port's GRO limits, `gro_max_size` and `gro_ipv4_max_size`, raised from 64 KB to 256 KB, so GRO could hand TCP larger aggregates and the per-skb costs (`tcp_v4_rcv`, `svc_data_ready`, the loan's walk) would be paid fewer times a WRITE.
- The loan's receive walks a `frag_list` chain (`svc_tcp_rx_walk_skb` through `skb_walk_frags`), and KUnit covers nested chains, so a larger aggregate takes a tested path.
- The A/B used `twoport.sh`'s `GRO_MAX=`. `settled.sh` held each run to it (`EXP_GRO_MAX`) and restored 64 KB afterwards.

**Run 2026-10-24 on tardis2**, build 494, round 05's sunrpc (`91F9753F3DA76DEF91A628D`), the settled setting of rounds 01, 03 and 08. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run27.sh`: A B A B A B untraced, a profile of B (`fl9-loan64`) with `dready.bt` beside it, then latency A B A B with `lat.sh`. `run28.sh` then measured what GRO hands TCP, one run each way with `xeu-rig/grolen.bt`.

## Outcome: not kept. No change, because the server gets nothing larger to merge

| run | GRO max | ring µs a WRITE | nfsd µs a WRITE | refusals | TCP segments in / out a WRITE |
|---|---|---|---|---|---|
| r9-1-loan64-A | 64 KB | 61.0 | 154.2 | 1.06 | 20.6 / 12.2 |
| r9-2-loan64-B | 256 KB | 63.6 | 156.4 | 0.94 | 20.3 / 13.0 |
| r9-3-loan64-A | 64 KB | 65.4 | 155.4 | 1.02 | 20.4 / 12.8 |
| r9-4-loan64-B | 256 KB | 63.7 | 155.7 | 0.95 | 20.6 / 13.3 |
| r9-5-loan64-A | 64 KB | 63.4 | 154.7 | 1.03 | 20.4 / 12.4 |
| r9-6-loan64-B | 256 KB | 63.0 | 154.2 | 1.01 | 20.4 / 12.7 |

Every run was untraced, settled, DRIVE-LIMITED (40.2-40.5 GB/s), with no drive stepped down.

- **The rings:** 63.3 against 63.4 µs a WRITE; **nfsd:** 154.8 against 155.4. Both inside the runs' scatter.
- **TCP's skbs in a WRITE** (`TcpInSegs`, which here counts one an aggregate) did not move: 20.4 both ways.
- **The profile** (fl8 at 64 KB, fl9 at 256 KB): every per-skb frame costs the same. `tcp_v4_rcv` 1.37 against 1.30 µs a WRITE on the rings, `skb_gro_receive` 2.83 against 2.85, `inet_gro_receive` 2.55 against 2.56. `dready.bt` counted 41.6 million `svc_data_ready` calls against round 05's 40.9 million for the same number of loans.

**Why** ([grolen.txt](grolen.txt), the length and segment count of each skb at `tcp_v4_rcv`):

| GRO max | aggregates | 32-64 KB | 64-128 KB | segments, most |
|---|---|---|---|---|
| 64 KB | 43.3 million | 36.0 million | 0 | 8-9 |
| 256 KB | 43.5 million | 36.1 million | 26,642 (0.06%) | 8-9 |

The limit was not what bounded the aggregates. An aggregate ends at 8-9 frames of 7740 bytes either way, about 62 KB, because that is what the client sends at once. Its port, `ens3np0`, has `gso_max_size` and `gso_ipv4_max_size` 65536, so each TSO burst is at most 64 KB, and the server's GRO does not merge across the client's bursts (a burst's last segment can carry PSH, which ends an aggregate). That the 26,642 larger aggregates exist at all shows the server's limit was in force. The client's hardware takes more (`tso_max_size` 524280).

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p99 | mean |
|---|---|---|---|
| A1, 64 KB | 0.457 ms | 0.635 | 0.469 |
| B1, 256 KB | 0.449 | 0.618 | 0.463 |
| A2, 64 KB | 0.449 | 0.651 | 0.466 |
| B2, 256 KB | 0.457 | 0.659 | 0.472 |

No difference.

## The wake, measured (L0's open question)

`lat.bt` now times the wake as the scheduler sees it, from an nfsd thread's `sched_wakeup` to the `sched_switch` that runs it ([latency.txt](latency.txt)):
- 98.5% of wakes take under 2 µs, and 0.04% over 32 µs, in all four runs. A wake is not where a WRITE's latency goes.
- There are many of them, though: about 5.8 million in a traced run of about 0.3 million WRITEs, roughly 19 a WRITE at iodepth 1. That is round 10's subject.

## What it leaves (Mike to decide)

BIG TCP only helps if the client sends bursts above 64 KB: `gso_ipv4_max_size` (and `gso_max_size`) raised on the client's `ens3np0` as well as the server's GRO limits. The client is shared with tardis1, so that would be a per-run client setting restored afterwards, not part of `settled.sh`'s server state. The server's limits stay at 64 KB.
