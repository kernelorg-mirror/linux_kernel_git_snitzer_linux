# Round 05: the `svc_data_ready` path

**The changes:** two commits, tested together, because each alone is below what three runs a side resolve.
- "SUNRPC: Read XPT_DATA before setting it in svc_data_ready()" (`eff5f16d4f30`).
- "SUNRPC: Stamp enqueue and wake times only while they are traced" (`0f0ed4159a12`). `xpt_qtime` and `rq_qtime` are taken only while `svc_xprt_dequeue` is enabled, the only reader. `rq_qtime` was a store into another thread's `svc_rqst` on every wake attempt.

**Run 2026-10-23 on tardis2**, build 494, in the settled setting with rounds 01 and 03.
- A is round 02's sunrpc: `refs-sunrpc.ko`, srcversion `1CC95EF56506263BC30D23B`.
- B is A plus both changes: `r5-sunrpc.ko`, srcversion `91F9753F3DA76DEF91A628D`.
- The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run22.sh`.
- Round 04 (the region pools' stack) waits for the next kernel build, so it was skipped.

## Why: round 02's ranking and a count

`svc_data_ready` cost the rings 7.73 µs a WRITE. The disassembly of the module that was profiled shows 93% of its own 2.05 µs on the transport's `xpt_flags` line:
- the `test_and_set_bit(XPT_DATA)` (an `ldsetal`): 1.28 µs;
- the first read of the line: 0.62 µs.

Under it, `svc_xprt_enqueue` cost 5.4 µs, of which:
- the wake: 3.3;
- two `ktime_get()` stamps: 0.68;
- the pool counters: 0.73.

`dready.bt` counted one loan run (`dready.txt`, 2.10 million WRITEs):

| | a WRITE |
|---|---|
| `svc_data_ready` calls | 19.4 |
| ↳ `XPT_DATA` already set | 4.6 (23.5%) |
| ↳ `XPT_DATA` clear | 14.9 |
| `svc_xprt_enqueue`, every caller | 13.0 |

So the first change avoids the atomic on only a quarter of calls.

## Outcome: kept. The rings 5% cheaper a loaned WRITE

**KUnit on B:** 7, 50, 7 and 10 pass, with no failure and no skip.

| run | GB/s | ring softirq | ring µs a WRITE | nfsd µs a WRITE | wakes a WRITE | refusals |
|---|---|---|---|---|---|---|
| r5-1-loan64-A | 40.43 | 64.7% | 67.1 | 159.5 | 6.32 | 0.96 |
| r5-2-loan64-B | 40.43 | 63.4% | 65.7 | 155.7 | 6.56 | 0.98 |
| r5-3-loan64-A | 40.35 | 67.2% | 70.0 | 161.6 | 6.41 | 0.92 |
| r5-4-loan64-B | 40.44 | 62.5% | 64.9 | 157.7 | 6.54 | 0.98 |
| r5-5-loan64-A | 40.47 | 65.6% | 68.0 | 157.9 | 6.33 | 1.03 |
| r5-6-loan64-B | 40.46 | 62.1% | 64.3 | 155.8 | 6.50 | 1.01 |

The runs were untraced, A B A B A B, each after a reload, settled, DRIVE-LIMITED, with no drive stepped down.

- **The rings:** 68.4 to 65.0 µs a WRITE, -3.4 µs (-5%), with no overlap. The refusals were the same.
- **nfsd:** 159.7 to 156.4 µs a WRITE, -3.3.

**The profiles before and after** (round 03's fl3 against this round's fl5):

| | the rings, µs a WRITE | nfsd, µs a WRITE |
|---|---|---|
| `ktime_get` | 1.05 → 0.58 | 2.09 → 1.26 |
| `svc_pool_wake_idle_thread` | 2.57 → 2.42 | 6.31 → 4.88 |
| `svc_data_ready` | 5.94 → 5.86 | |

- The first change is too small to show in the profile, as the count said it would be.
- The profiles directly account for about 0.6 µs on the rings and 2-3 on nfsd. The A/B shows more on the rings, -3.4.
- The two profiles are single runs taken at different times, so the A/B is the evidence here.
- The store into another CPU's `svc_rqst` may cost the receiving side more than its own samples show. That is a guess.

**Since the start of the rounds:** a loaned WRITE costs the rings about 65 µs, against 92.6 before round 01 (-30%).

## Corrections to earlier rounds, found in this one

- **Round 01's items 3 and 4 were one cost, counted twice.**
  - `napi_consume_skb` (item 4, called "transmit completions") is called 99.9% from `skb_defer_free_flush` (item 3), so it is part of it: the deferred frees of received skbs.
  - Round 03 removed both from the rings.
  - Transmit completions (`xeu_tx_poll`) run on port a's odd ring CPUs (1, 3, 5, 7), which the profile's ring group leaves out.
- **xeu's `rx_packets` is not wire frames.** It counts 41.7 a 1 MiB WRITE (run r5-4), about 25 KB each, with LRO and hardware GRO off. So the "135 frames a WRITE" behind X8's "42 ns a frame" in `xeu-rig/XSIGHT-FOLLOWUP.md` is unfounded, and X8 now gives µs a WRITE only.
- **ACKs:** 3.9 packets a WRITE are sent (run r5-4), about one reply and 2.9 ACKs, not one ACK each pass.
  - `tcp_send_ack` cost nfsd 17.5 µs a WRITE in round 03, about 6 µs an ACK. About half of it is the qdisc (round 07).
  - Round 06 counts segments with TCP's own counters.
