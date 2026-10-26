# Round 26: an nfsd thread sleeps without a timeout when its pool cannot lose it

**What was profiled:** fl48-loan64, round 25 at the settled setting (`svc_tcp_rx_want_eighths` 6). nfsd cost 100.7 µs a WRITE.

The round looked at two things before the change.

## The refused pass: looked at, not pursued

Round 25 left a pass at a record's start that reads 32 to 48 bytes and is then refused region credit. `refused.bt` ([refused.txt](refused.txt)) ran on two loan64 runs on build 499's own sunrpc, 10 s each:

| per 10 s | p1 | p2 |
|---|---|---|
| records admitted | 335,336 | 338,327 |
| admitted only after a refusal | 334,560 (99.8%) | 333,346 (98.5%) |
| refused once / twice | 317,262 / 17,295 | 317,183 / 16,163 |
| why: avail short of the cost / a waiter ahead | 69% / 31% | 69% / 31% |
| refused passes | 351,739 | 349,397 |

- **A refusal is how the budget paces every loaned WRITE.** At each one, 56 to 72 records are in flight against the 64-WRITE budget. Every record is 1 MiB, the cost asked 64 to 256 pages, and avail 64 to 256.
- **The wait:** from refusal to admission, mostly 128 to 256 µs, the time for a record to end. By then 256 to 512 KiB of the record has usually queued, and the admitting pass reads about 400 KB.
- **The refused pass:** about 1.05 a WRITE. It reads about 30 bytes and sends 0.6 ACKs, and takes about 12 µs, the same as an ordinary pass (the wake, the switch, the socket lock, the release's backlog, the ACK, and the probe's own cost). Deciding the refusal is cheap: `svc_tcp_rx_credit_admit_to` is 0.35 µs a WRITE in fl48.
- **The two fixes, and why neither was taken (Mike, 26 October):**
  - Not consuming the header on refusal saves only the ACK: under 1 µs a WRITE, for a change to the receive state machine at a record's start.
  - Skipping the pass would save 3 to 5 µs a WRITE (estimated from per-pass costs, not measured). It means putting a transport on the waiter list before its record's size is known, when its previous record ends. Small calls would then wait for credit they do not need, and a waiter whose record never needs credit would have to be let go, the case that once stranded every transport behind it.

## The wake: the timer is ours

The two top self frames on nfsd in fl48 are mostly time the profiler charges where interrupts are switched back on ([top-nfsd.txt](top-nfsd.txt)):
- `_raw_spin_unlock_irqrestore`, 4.81 µs a WRITE: 2.0 is thread wakes (`wake_up_process` from `svc_pool_wake_idle_thread()`, 1.10 from an enqueue, 0.89 from a thread waking the next), 0.86 NVMe request setup, about 0.5 the idle sleep's timer.
- `finish_task_switch`, 3.35: 2.83 is nfsd threads going to sleep in `schedule_timeout()`.
- **The wake itself is the scheduler's:** about 0.5 µs a wake, about 4 a WRITE. The host has one 32 MiB L3 over all 64 CPUs, so the waker locks the target's run queue and queues the task itself. Fewer wakes is round 18's direction, which cost latency.
- **The timer:** nfsd sleeps with `svc_recv(rqstp, 5 * HZ)`, so every idle sleep arms a 5 s timer and deletes it when the thread is woken microseconds later. The timeout is there only so an idle thread above its pool's minimum can exit, and `svc_recv()` reports it only when `sp_nrthrmin` is set. A probe showed `sp_nrthrmin` 0 with 512 threads: the timer is pure cost here. In fl48 it is `add_timer` 0.30, `timer_delete_sync` 0.20 and the rest of `schedule_timeout()` outside `schedule()`, about 0.5 µs a WRITE.

## The change

- **"SUNRPC: Sleep without a timeout when the pool cannot lose the thread"** (`6c6dc284fed0`): `svc_pool_can_shrink()` is the test `-ETIMEDOUT` already made (a minimum, and more threads than it). `svc_recv()` passes the timeout to the idle sleep only when it holds, and sleeps without a timer otherwise.
- The `-EBUSY` path that spawns threads still tests nfsd's own timeout.
- The pool grows past its minimum only when `svc_recv()` finds no idle thread, so no thread asleep without a timer is left behind by the growth. A thread asleep when the minimum is lowered may go from its next idle sleep.
- No page is held longer, so the region is not touched.

**KUnit:** the first run of the A/B stopped at KUnit, before any measured run, on round 25's test. `svcsock_rx_set_want_test` expected half the window whatever `svc_tcp_rx_want_eighths` the running kernel had, and failed at the settled 6 (768 KiB against 512). Round 25's KUnit had passed because it ran while the parameter was still 4.
- `ae6b4ecdc0d2` (KUnit interface): `svc_tcp_rx_kunit_want_eighths()` sets the share and returns the one it replaces.
- `05e5af5c3d3f` (KUnit tests): the test sets 4 and puts the kernel's share back after, and also checks 6 (384 KiB of a 512 KiB window) and 9, clamped to the whole window.
- With them: 7, 52, 7 and 10 pass.

**Runs 2026-10-26 on tardis2**, build 499 (`bca1a2f143eb`), the settled setting, nfsd the kernel's own (`3F0C341E86E266D5FDF2ADF`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run50.sh`, the runs `/var/tmp/zc-run-logs/t2-r49/`.
- A is round 25's sunrpc (`FD78696DEDB1901026A0DAD`), B the change with the test helper (`A06FE478658242A10E100FF`, built by `build-sunrpc.sh`, all 327 exports at their versions).
- Before every run sunrpc was swapped and the host settled; no load raised a new WARNING.

## Outcome: kept. The timer gone from the profile; too small for the A/B

| run | sunrpc | GB/s | ring µs a WRITE | nfsd µs a WRITE | segments sent a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|
| r50-1-loan64-A | round 25 | 40.51 | 47.0 | 99.0 | 5.59 | 6505 |
| r50-2-loan64-B | no timer | 40.49 | 48.4 | 98.4 | 5.54 | 6412 |
| r50-3-loan64-A | round 25 | 40.59 | 46.9 | 98.6 | 5.46 | 6503 |
| r50-4-loan64-B | no timer | 40.50 | 46.6 | 97.2 | 5.52 | 6348 |
| r50-5-loan64-A | round 25 | 40.53 | 45.5 | 96.8 | 5.48 | 6537 |
| r50-6-loan64-B | no timer | 40.49 | 48.6 | 95.4 | 5.54 | 6467 |
| r50-7-loan64-A | round 25 | 40.50 | 46.4 | 94.4 | 5.53 | 6425 |
| r50-8-loan64-B | no timer | 40.49 | 48.1 | 95.1 | 5.48 | 6526 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt)). No run stalled: the slowest WRITE of each took 110 to 126 ms.

| per loaned WRITE | A | B | B - A |
|---|---|---|---|
| nfsd, µs | 97.20 ± 2.10 | 96.53 ± 1.56 | -0.67 ± 1.31 |
| rings, µs | 46.45 ± 0.69 | 47.92 ± 0.91 | +1.47 ± 0.57 |
| segments sent | 5.52 | 5.52 | 0 |
| enqueues · wakes · switches | 4.73 · 3.95 · 5.42 | 4.74 · 3.96 · 5.44 | 0 |

- **The profile** (fl50-loan64 on B against fl48-loan64): inside nfsd's idle sleep `add_timer` 0.30 to 0.00, `timer_delete_sync` 0.20 to 0.00, `schedule_timeout()` outside `schedule()` 0.53 to 0.01. The sleep inclusive 3.04 to 2.73. About 0.5 µs a WRITE of work is gone.
- **The A/B cannot see it:** both sides fell about 5 µs over the session, 99.0 to 94.4, more than the change. nfsd -0.67 is the right size and half a standard error.
- **The rings:** +1.47 in the A/B, three pairs of four up. But the change does not run on the ring CPUs and nothing timer-related is there (`run_timer_softirq` 0.01 in both profiles), and B's profile has the rings at 48.24 against fl48's 52.66. Taken as scatter.
- **The profiles' totals:** nfsd 100.67 and 102.49, one profiled run each a session apart, with 2 to 4 µs between runs.
- **Not changed:** the rate, the region's peak, refused loans, segments, enqueues, wakes and switches. The change takes a timer out of the sleep, not a sleep.
- The Rx and Tx interrupts in [cost.txt](cost.txt) are not valid, as in round 21.

## Latency

[latency.txt](latency.txt), untraced runs at iodepth 1:

| | GB/s | p50 | p90 | p99 | mean |
|---|---|---|---|---|---|
| B1, no timer | 7.77 | 453 µs | 522 | 586 | 464 |
| A1, round 25 | 7.78 | 469 | 518 | 578 | 456 |
| B2, no timer | 7.82 | 465 | 510 | 569 | 453 |
| A2, round 25 | 8.02 | 432 | 510 | 578 | 445 |

- The same.

## Decision

- **Kept (Mike, 26 October).** It is code with no parameter, so `settled.sh` has nothing to hold. It does not touch the loan series and stands as a patch of its own for upstream.
- Its saving grows with how often threads sleep: small calls gain more than 1 MiB WRITEs.
