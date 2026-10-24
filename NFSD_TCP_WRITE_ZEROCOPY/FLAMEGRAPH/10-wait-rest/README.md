# Round 10: wait for the rest of a loaned record in `svc_data_ready()`

**The change** (round 06's alternative, 06b):
- "SUNRPC: Wake no thread for less than a loaned TCP record lacks" (`c0995ea94798`):
  - After a read that drained the socket while a record holding loan credit is incomplete, the transport records what the record lacks (`sk_rx_want`).
  - `svc_data_ready()` sets `XPT_DATA` only once `tcp_epollin_ready()` is true for that amount. That test is also true under receive memory pressure and with the window nearly closed, and a FIN always wakes a thread.
  - `sk_rcvlowat` stays 1, so TCP keeps delaying its ACKs: round 06 failed on the ACKs TCP sends for every skb below the mark.
- "SUNRPC: Hold a loaned TCP record back for at most half the open window" (`dd4fef2000a0`, round 10b): what is wanted is capped at half of the receive window still open after the read, not half the receive buffer.
- KUnit: `eec87694362c` exposes `svc_tcp_rx_set_want()`, and `756c90dfd01b` and `b2bd2c43b306` test it and the hold in `svc_data_ready()`. 7, 52, 7 and 10 pass on both modules.

**Runs 2026-10-24 on tardis2**, build 494, the settled setting.
- A is round 05's module (`91F9753F3DA76DEF91A628D`).
- B is `91B0ED4CD08083AC80139B8` (round 10) in `run29.sh`, and `6D7E259F6D29200D03F15FC` (round 10b) in `run30.sh`. Both are in `/var/tmp/zc-run-logs/t2-tipkernel/`.
- Every run reloaded sunrpc first. TCP's segments, retransmits, delayed ACKs and zero-window adverts were read around each run.

## Round 10: half the receive buffer. Not kept: the client waits

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE | woken a WRITE | refusals | TCP segments out a WRITE |
|---|---|---|---|---|---|---|
| r10-1-loan64-A | 40.49 | 62.8 | 154.2 | 6.44 | 1.02 | 12.8 |
| r10-2-loan64-B | **37.88** | 57.2 | 108.6 | 3.68 | 0.07 | 5.2 |
| r10-3-loan64-A | 40.49 | 64.9 | 154.8 | 6.55 | 0.98 | 12.9 |
| r10-4-loan64-B | **35.94** | 53.5 | 118.9 | 3.73 | 0.03 | 5.2 |
| r10-5-loan64-A | 40.37 | 65.7 | 157.8 | 6.58 | 0.94 | 13.1 |
| r10-6-loan64-B | **36.99** | 57.6 | 108.1 | 3.70 | 0.06 | 5.2 |

- It did what it was for: a record took 3.75 passes and 5.5 hand-offs (round 05: 12 and 22), and nfsd was 28% cheaper a WRITE.
- But the rate fell 7-11%, and at iodepth 1 about one WRITE in seven waited 4-5 ms: p90 4.3-4.6 ms against 0.56-0.58, and the client's rate halved.
- **Why:** the server's sockets had a receive buffer (`rb`) of 768000, so up to 384 KB was wanted, but each connection advertised only about 300-350 KB of window (`rcv_ssthresh` about 355K). What was wanted could not arrive. A thread was woken only when the window had all but closed and the client had stopped sending, and the client then waited for the read's window update.

## Round 10b: half the open window. Kept: nfsd 17% cheaper, latency no worse

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE | woken a WRITE | refusals | TCP segments out a WRITE |
|---|---|---|---|---|---|---|
| r10w-1-loan64-A | 40.41 | 62.7 | 152.1 | 6.58 | 0.93 | 13.1 |
| r10w-2-loan64-B | 40.51 | 60.9 | 127.5 | 4.47 | 1.06 | 6.5 |
| r10w-3-loan64-A | 40.47 | 63.7 | 156.9 | 6.55 | 0.98 | 12.9 |
| r10w-4-loan64-B | 40.50 | 60.4 | 129.3 | 4.56 | 1.05 | 6.5 |
| r10w-5-loan64-A | 40.47 | 62.9 | 152.3 | 6.46 | 1.02 | 12.4 |
| r10w-6-loan64-B | 40.47 | 62.4 | 125.6 | 4.58 | 1.05 | 6.6 |

Every run was settled, DRIVE-LIMITED, with no drive stepped down; no retransmits, delayed-ACK timeouts or zero-window adverts either way.

- **nfsd:** 153.8 to 127.5 µs a WRITE (-26.3, -17%), with no overlap.
- **The rings:** 63.1 to 61.2 (-3%).
- **TCP:** half the segments out (12.8 to 6.5 a WRITE). Unlike round 06, where the ACKs moved onto the rings, here they are simply fewer.
- **The profile** (fl8, round 05's module, against fl10w):
  - A record takes 5.4 passes and 8.8 hand-offs, against 12 and 22.
  - On nfsd, `svc_tcp_recvfrom` 63.2 to 47.2 µs a WRITE, `tcp_cleanup_rbuf` 16.9 to 9.0, `svc_thread_wait_for_work` 5.3 to 3.7.
  - On the rings, `svc_data_ready` 4.9 to 4.4.

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, round 10b | 0.444 ms | 0.537 | 0.635 | 0.459 |
| A1, round 05 | 0.528 | 0.594 | 0.692 | 0.531 |
| B2, round 10b | 0.461 | 0.537 | 0.602 | 0.469 |
| A2, round 05 | 0.461 | 0.569 | 0.643 | 0.477 |

- p90 is 0.537 in both B runs against 0.569-0.594 for A. The rest is within the runs' scatter.
- At iodepth 1, nfsd threads were woken about 13.4 times a WRITE against 20.4 (`sched_wakeup`, from `lat.bt`).

**Since the start of the rounds:** a loaned WRITE costs the rings about 61 µs (92.6 before round 01) and nfsd about 128 (184 in round 01's profile).

**Left as it is:**
- Round 10b is a separate commit on top of round 10. Folding it in is for the next regroup.
- tardis2 runs round 10b's module (`6D7E259F6D29200D03F15FC`) from here on, as the base for the next rounds.
