# Round 07: the server port's qdisc, `pfifo_fast` against `fq_codel`

**The change:** each of port a's transmit queues gets `pfifo_fast` instead of the settled `fq_codel`.
- `pfifo_fast` is lockless (`TCQ_F_NOLOCK`) and lets a packet skip an empty queue.
- It is a setting, so the test needed no reload. `twoport.sh` takes `SRV_QDISC=`, and `settled.sh` audits the port's qdisc, holds a run to `EXP_SRV_QDISC`, and puts it back on apply. `twoport.sh` also puts it back when a run ends: a gap found in this round, when the latency run's second half found `pfifo_fast` left behind and refused to start.

**Run 2026-10-24 on tardis2**, build 494, round 05's sunrpc (`91F9753F3DA76DEF91A628D`), the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run25.sh`.

## Why

In round 03's profile, half of each ACK nfsd sent was `__dev_xmit_skb` (8.1 µs a WRITE). It was taken to be the qdisc: `fq_codel` and its lock.

## Outcome: not kept, no change

| run | qdisc | ring µs a WRITE | nfsd µs a WRITE | TCP segments out a WRITE | refusals |
|---|---|---|---|---|---|
| r7-1-loan64-A | fq_codel | 67.9 | 157.4 | 13.01 | 1.00 |
| r7-2-loan64-B | pfifo_fast | 63.8 | 155.7 | 12.71 | 1.01 |
| r7-3-loan64-A | fq_codel | 67.3 | 156.1 | 12.68 | 1.00 |
| r7-4-loan64-B | pfifo_fast | 67.1 | 161.0 | 13.16 | 0.95 |
| r7-5-loan64-A | fq_codel | 65.5 | 157.3 | 12.87 | 1.03 |
| r7-6-loan64-B | pfifo_fast | 67.3 | 157.2 | 12.96 | 1.01 |

- **The rings:** 66.9 against 66.1 µs a WRITE.
- **nfsd:** 156.9 against 158.0.
- **Latency at low depth with `pfifo_fast`** (L7, untraced): p50 0.465 ms, p99 0.651, mean 0.479. That is inside L0's own scatter (p50 0.457-0.522).

**Why it did nothing.** In the profiles before and after (fl5 with `fq_codel`, fl7 with `pfifo_fast`), nfsd's `__dev_xmit_skb` is 10.3 and 9.7 µs a WRITE:

| under `__dev_xmit_skb` | fq_codel | pfifo_fast |
|---|---|---|
| `_raw_spin_lock` | 2.59 | 2.75 |
| `sch_direct_xmit` → the driver | 2.53 | 2.64 |
| its own code | 2.23 | 2.12 |
| the qdisc's own enqueue and dequeue | 0.64 | 0.29 |

The qdisc's algorithm was never the cost (0.64 µs). What remains with either one is the transmit path's lock and the per-packet work around the driver.
- That lock serializes nfsd's 48 CPUs onto port a's 4 transmit queues, 12 CPUs a queue through XPS.
- The lever would be more transmit queues for nfsd's sends. Those are tied to the port's rings (`RINGS=4` combined, receive on CPUs 0, 2, 4 and 6), so it is a change of the ring layout, not a setting. It is noted and not queued.
