# Round 25: an incomplete loaned record taken up again at three quarters of the window

**What was profiled:** fl46-loan64, round 24's B at the settled setting. nfsd cost 106.4 µs a WRITE.
- The round started on the transmit lock: `__dev_xmit_skb` 4.87 µs a WRITE on nfsd. It is not contended: `_raw_spin_lock` 1.31, `queued_spin_lock_slowpath` 0.09, so a miss on the queue's lock line, not a wait. More transmit queues or another XPS map would only spread that miss, and eight channels would add receive rings and region pools. Not pursued.
- 3.58 of the 4.87 were ACKs, not replies (1.28), and all of them sent by nfsd's own read: `tcp_read_sock()` ends every pass in `tcp_cleanup_rbuf()`, whose `tcp_send_ack()` is 7.98 µs a WRITE in all. The rings send almost none (0.13).

**Why the read ACKs** ([probes.txt](probes.txt), on build 499's own sunrpc; rounds 21 to 24 do not change how TCP acknowledges):
- `acks.bt`: 2.0 M reads on port 2049 in 10 s, 1.82 M ACKs, about 4.8 a WRITE. 98% by the rule "more than one rcv_mss unacknowledged", as each pass takes well over one 7.8 KB segment; 2% window updates.
- With each ACK's read size: 1.63 M after a read of 4 KiB or more (mostly 128 to 512 KiB), 0.22 M after one under 4 KiB.
- `small.bt` (fentry/fexit: a kretprobe recorded nothing, as the hundreds of threads asleep in `svc_tcp_recvfrom` on the socket lock outrun its instances): the small passes are a record's first 32 to 48 bytes, read before its loan credit is refused (350,291 in 10 s, 62% of them ACKed, about 0.57 ACKs a WRITE), and whole 128 to 144 byte records (44,447). Most ACKs come from the body passes: 1.08 M that leave a record incomplete, 0.35 M that complete it.
- So a 1 MiB loaned record is taken up in about four passes. Round 10b wakes a thread for an incomplete loaned record once half of the receive window still open has arrived; each pass is a wake, the socket lock, hand-offs and an ACK.

## The change

- **"SUNRPC: Make the share of the window a loaned record waits for tunable"** (`2a75926c1458`): `svc_tcp_rx_set_want()` waits for what the record lacks or `svc_tcp_rx_want_eighths` eighths of the open window, whichever is less. The default, 4, is the half round 10b set.
- `settled.sh` holds it, `twoport.sh` takes `WANT_EIGHTHS=` (`0b2a486c5bcd`), and since this round `settled.sh` holds it at 6 (`dcfe29109e60`).
- The share not waited for is what the client can still send while the woken thread gets going. Waiting for all of it is round 10's stall.

**Runs 2026-10-26 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The drivers are `/var/tmp/zc-run-logs/t2-tipkernel/run48.sh` and `run48b.sh`, the runs `/var/tmp/zc-run-logs/t2-r47/`.
- One sunrpc for every run, the tip with rounds 21 to 24 and the parameter (`FD78696DEDB1901026A0DAD`): at 4 it is round 24's B. So sunrpc was swapped once before the runs and once after, and each run set its share; `settled.sh` audited each against it.
- KUnit: 7, 52, 7 and 10 pass. No new WARNING.
- run48.sh was stopped after its first latency run at 7 (Mike: "No sense wasting 25 min on runs that are useless"); run48b.sh ran the rest at 4 and 6.

## Outcome: 6 kept and settled. nfsd 5.3 µs cheaper a WRITE, one ACK fewer

**7 is out** ([latency.txt](latency.txt)): at iodepth 1 it held WRITEs until about 4 to 5 ms, the rate 3.88 GB/s against 7.5 to 8.1. With four WRITEs in flight a record often cannot get 7/8 of the open window: the client's congestion window and the ACKs the server holds back stop it short, and nothing takes the record up until the delayed-ACK timer.

| run | want_eighths | GB/s | ring µs a WRITE | nfsd µs a WRITE | segments sent a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|
| r48-1-loan64-w4 | 4 | 40.49 | 52.1 | 104.1 | 6.61 | 6404 |
| r48-2-loan64-w6 | 6 | 40.51 | 47.0 | 97.7 | 5.53 | 6477 |
| r48-3-loan64-w4 | 4 | 40.49 | 51.5 | 102.1 | 6.51 | 6460 |
| r48-4-loan64-w6 | 6 | 40.52 | 49.2 | 99.5 | 5.60 | 6297 |
| r48-5-loan64-w4 | 4 | 40.45 | 49.9 | 104.2 | 6.57 | 6407 |
| r48-6-loan64-w6 | 6 | 40.49 | 50.6 | 97.5 | 5.64 | 6198 |
| r48-7-loan64-w4 | 4 | 40.51 | 50.7 | 101.9 | 6.54 | 6587 |
| r48-8-loan64-w6 | 6 | 40.58 | 48.5 | 96.4 | 5.54 | 6488 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)). No run stalled: the slowest WRITE of each took 109 to 122 ms. Segments sent are the server's TCP OutSegs a WRITE over the run.

| per loaned WRITE | 4 | 6 | 6 - 4 |
|---|---|---|---|
| nfsd, µs | 103.08 ± 1.24 | 97.78 ± 1.28 | **-5.30 ± 0.89 (-5%)** |
| rings, µs | 51.05 ± 0.96 | 48.83 ± 1.50 | -2.22 ± 0.89 |
| segments sent | 6.56 | 5.58 | -0.98 |
| enqueues | 5.46 | 4.79 | -0.67 |
| wakes | 4.38 | 3.99 | -0.39 |
| voluntary switches | 5.86 | 5.46 | -0.40 |

- **nfsd:** about six standard errors; every run at 6 (96.4 to 99.5) is below every run at 4 (101.9 to 104.2).
- **How:** one ACK a WRITE fewer, and fewer enqueues, wakes and switches with it; each count is within a few hundredths between runs.
- **The profile** (fl48-loan64 at 6 against fl46-loan64, the same code at 4, [top-nfsd.txt](top-nfsd.txt)): nfsd busy 106.36 to 100.67; `svc_tcp_recvfrom` 34.89 to 31.09 inclusive, the read's ACKs 7.98 to 6.55, the idle sleep 4.11 to 3.78.
- **The rings:** -2.2, about 2.5 standard errors: fewer ACKs to complete and fewer data-ready wakes.
- **The region:** at 6 the peaks are 6198 to 6488, at 4 6404 to 6587: not raised. The loan budget, not the wake, bounds what is held.
- **Not changed:** the rate, refused loans.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## Latency

[latency.txt](latency.txt), untraced runs at iodepth 1:

| | GB/s | p50 | p90 | p99 | mean |
|---|---|---|---|---|---|
| 6, run 1 | 7.86 | 453 µs | 510 | 569 | 456 |
| 4, run 1 | 7.46 | 436 | 510 | 578 | 444 |
| 7 | 3.88 | 465 | 4178 | 5276 | 997 |
| 6, run 2 | 8.11 | 436 | 502 | 569 | 445 |
| 4, run 2 | 7.85 | 453 | 522 | 578 | 453 |

- 6 and 4 the same. 4's first run had one WRITE of 51 ms; no other run at 4 or 6 had one over 1.2 ms.

## Decision

- **6 kept and settled (Mike, 26 October):** `settled.sh` holds `svc_tcp_rx_want_eighths` at 6 and `apply` writes it; it is not in `/etc/modprobe.d/sunrpc.conf`, which build 499's own sunrpc, without it, would warn about at every load. The kernel's default stays 4.
- Copy mode is not touched: only a loaned record with credit waits for a share of the window.
- **Left:** the refused pass at a record's start (about 0.57 ACKs a WRITE, a header read again when credit returns), and 6 on two ports, not measured.
