# Round 31: the server port with no qdisc

**What was profiled:** fl60-loan64, the current state (`/var/tmp/zc-run-logs/t2-r59/`): the tip's sunrpc (round 26's build, `A06FE478658242A10E100FF`), nfsd the kernel's, `rodata=noalias` (round 30), xeu `rx_prefetch` Y (round 28), the settled setting. Rings 43.25, nfsd 96.46 µs a WRITE ([fl60-cost.txt](fl60-cost.txt), [top-nfsd.txt](top-nfsd.txt), [top-rings.txt](top-rings.txt)).
- **nfsd's largest loan frame, `svc_tcp_rx_append_ref`** (2.42 µs a WRITE, [annotate-svc_tcp_rx_append_ref.txt](annotate-svc_tcp_rx_append_ref.txt)), is not one stall. It is about 128 appends a WRITE at about 19 ns each. The time is spread over the loads of the record state's `borrowed` and `page_pool` bitmaps and of each page's pool (`page_pool_page_in_rx_region()`). Not worth a round alone.
- **Sending costs nfsd 14.9 µs a WRITE** (`svc_send`):
  - the reply, `svc_tcp_sendmsg`, 7.0;
  - the ACKs nfsd's own reads send, `tcp_cleanup_rbuf` → `tcp_send_ack`, 6.4.
- **Under every send, `__dev_xmit_skb`, 4.24 µs a WRITE:**
  - the qdisc's root lock, 1.10;
  - its own code, 0.86;
  - the driver's transmit, `sch_direct_xmit`, 1.57;
  - the rest, the unlock and `__qdisc_run`.
- **Round 07** found the qdisc's algorithm was never the cost (`pfifo_fast` saved nothing). What every qdisc keeps is the root lock and the code around it.
- Other processes on nfsd's CPUs are under 4% of the profile's samples (Claude Code's own process 0.8%).

## The change

`noqueue` as the port's root qdisc, so a send goes to the driver under its queue's transmit lock, without a qdisc's lock or code.
- **A setting.** `noqueue` cannot be a class's, so `settled.sh`'s `set_srv_qdisc()` now puts it at the root, where it is every transmit queue's (`cd909d9ae8e2`). The settled qdisc stays `fq_codel` under the mq.
- **The risk:** with no queue, a send that finds the transmit queue stopped is dropped (`__dev_queue_xmit()`, "Virtual device ... asks to queue packet!"). The runs count the port's `tx_dropped` and those messages.

**Runs 2026-10-27 on tardis2,** the state above (driver [run63.sh](run63.sh), the runs `/var/tmp/zc-run-logs/t2-r63/`, [cost.txt](cost.txt)):
- A is `fq_codel`, B is `noqueue` (`SRV_QDISC=noqueue`, which twoport.sh sets and undoes).
- `lat.sh` B then A, then four untraced loan64 runs a side, A B A B A B A B, each after `settled.sh apply`.
- No WARNING.

## Outcome: not kept. `noqueue` drops the server's sends

| run | qdisc | GB/s | ring µs a WRITE | nfsd µs a WRITE | sends dropped | client retransmits | slowest WRITE, ms |
|---|---|---|---|---|---|---|---|
| r63-1-loan64-A | fq_codel | 40.52 | 46.4 | 93.0 | 0 | 5 | 110.1 |
| r63-2-loan64-B | noqueue | 40.26 | 45.2 | 88.0 | 581 | 17 | 299.9 |
| r63-3-loan64-A | fq_codel | 40.52 | 46.4 | 94.4 | 0 | 4 | 128.7 |
| r63-4-loan64-B | noqueue | 40.46 | 46.2 | 91.4 | 719 | 12 | 309.5 |
| r63-5-loan64-A | fq_codel | 40.49 | 42.1 | 93.2 | 0 | 6 | 117.7 |
| r63-6-loan64-B | noqueue | 40.46 | 46.1 | 90.5 | 101 | 5 | 118.2 |
| r63-7-loan64-A | fq_codel | 40.53 | 45.6 | 88.7 | 0 | 5 | 117.5 |
| r63-8-loan64-B | noqueue | 40.44 | 45.8 | 89.1 | 481 | 12 | 304.7 |

| per loaned WRITE | A, fq_codel | B, noqueue | B − A |
|---|---|---|---|
| nfsd, µs | 92.33 ± 2.49 | 89.75 ± 1.50 | −2.58 ± 1.46 |
| rings, µs | 45.13 ± 2.05 | 45.83 ± 0.45 | +0.70 ± 1.05 |
| segments sent | 5.56 | 5.55 | about 0 |
| enqueues · wakes · switches | 4.78 · 4.02 · 5.49 | 4.78 · 4.02 · 5.48 | 0 |

- **The drops:** every B run dropped sends, 101 to 719, and none of A's did.
  - Three of B's four runs had a WRITE of about 300 ms, against 110 to 129 in every A run. That is a dropped segment waiting for its retransmission timer.
  - The client retransmitted 12 to 17 times in those runs, against 4 to 6.
  - The drops are what xeu's `xeu_maybe_stop_tx()` causes: a transmit queue stopped for a moment, with nothing to hold the send. The server's sends are small, but 12 of nfsd's CPUs share each of the 4 queues.
- **nfsd:** −2.58 ± 1.46, about 1.8 standard errors. The pairs are −5.0, −3.0, −2.7 and +0.4. Suggestive, not shown. The A runs alone spread from 88.7 to 94.4.
- **The rings, rate, segments, wakes and region's peak** are the same.
- **Latency at iodepth 1** (untraced, before the drops show): `noqueue` p50 432 µs, mean 439; `fq_codel` 444 and 453. The same.

## Decision

- **Not kept:** a send dropped for a stopped queue costs a 300 ms WRITE, whatever the qdisc layer saves. `settled.sh` keeps `fq_codel`, and `set_srv_qdisc()` keeps `noqueue` for a later look.
- **What it suggests:** the qdisc layer may cost nfsd about 2.6 µs a WRITE. The lever round 07 named, more transmit queues for nfsd's 48 CPUs (now 4, tied to the port's rings), stands. `pfifo_fast` is the nearest qdisc to keeping both: lockless, and bypassed while empty. Round 07 found it saved nothing, which says the 2.6 here may be the runs' spread, not the qdisc layer.
