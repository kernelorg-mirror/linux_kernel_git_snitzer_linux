# Round 17: the wake path. Read on when the rest of a loaned record arrives during the read

**The change:** "SUNRPC: Read on when a loaned TCP record's rest arrives during a read" (`5fde85ef4076`).
- A thread that reads part of a loaned record and drains the socket sets what the record lacks (`sk_rx_want`, round 10). `svc_data_ready()` sets `XPT_DATA` again only once that much is queued.
- Segments that arrive while the thread holds the socket lock wait on the backlog. Their `svc_data_ready()` runs in `release_sock()` at the end of the thread's own read, and often finds the rest there.
- Until now the thread then parked the record's pages, handed the transport back, and the hand-back enqueued it and woke another thread to take the pages up again.
- Now, if `XPT_DATA` is set again by the time the lock is released, the thread clears it and reads on. The loop ends when the record completes or a read leaves `XPT_DATA` clear. It never runs for a record without loan credit, whose want is 0.

**Runs 2026-10-24 on tardis2**, build 494, the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run35.sh`.
- A is round 14's sunrpc (`FBEBC68D6628C0CE72E3E00`), B round 17's (`821534936DB054AC699BF52`). nfsd is round 13's on both.
- KUnit on B: 7, 10, 52 and 7 pass.

## What the wake path costs (round 14's profile and traces)

From fl14, in µs a WRITE:
- **On nfsd:** `svc_recv` outside `svc_handle_xprt` is 9.4. Of that, the sleep (`svc_thread_wait_for_work`) is 3.7 and the wakes it makes are 1.5. The wakes made by enqueues add another 1.5.
- **On the rings:** `svc_pool_wake_idle_thread` is 1.25.
- **A wake costs its waker about 1 µs.** `try_to_wake_up` runs with interrupts off, so its time shows as `_raw_spin_unlock_irqrestore`. With `fentry`/`fexit` on it, most wakes took 2-4 µs, probe included. The woken thread runs on the CPU it last ran on in 92% of wakes from nfsd.

[`xeu-rig/wakesrc.bt`](../../xeu-rig/wakesrc.bt) classes each call of `svc_pool_wake_idle_thread()` by where it comes from, and counts the passes through `svc_recv()` ([wakesrc.txt](wakesrc.txt)). Per WRITE, with round 14's sunrpc:

| wakes from | calls | threads woken |
|---|---|---|
| the ring's `svc_data_ready()` | 2.59 | 1.24 |
| `svc_thread_wake_next()`, more queued after a dequeue | 2.37 | 1.07 |
| the hand-back after a complete record | 1.06 | 0.48 |
| credit returned | 0.85 | 0.42 |
| the hand-back after a partial record | 0.56 | 0.27 |
| all | 7.5 | 3.5 |

- A call that wakes no one found the first idle thread's wake already in flight.
- **Passes through `svc_recv()`:** 8.7 a WRITE. 4.1 dequeue a transport; 4.4 find none.
  - 3.8 of the empty passes never slept: the thread saw a transport queued and another thread took it first.
  - 0.57 slept, were woken, and found nothing.
  - `probe9.bt` (in the run directory), which records each `svc_thread_should_sleep()` answer in a pass, splits the empty passes about 37% lost races, 30% woken for nothing and 25% woken before they slept.
- **What a hand-back leaves on the socket** (`probe11.bt`, in the run directory):
  - After a complete record, the next is nearly always queued: only 1.3% of those hand-backs set `XPT_DATA` on an empty socket. Their wakes are needed.
  - After a partial record, about one hand-back in seven found `XPT_DATA` set again. That is 0.6 a WRITE, each an enqueue, often a wake, a dequeue, and the pages parked and taken back up, to read what the first thread could read itself.

That last item is this round's change. The others are below.

## Outcome: kept. 0.6 fewer hand-offs a WRITE, nfsd 2% cheaper, latency the same

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE | enqueued a WRITE | woken a WRITE | voluntary switches a WRITE |
|---|---|---|---|---|---|---|
| r17-1-loan64-A | 40.50 | 59.6 | 115.0 | 6.09 | 4.62 | 6.12 |
| r17-2-loan64-B | 40.52 | 58.4 | 113.2 | 5.39 | 4.32 | 5.82 |
| r17-3-loan64-A | 40.52 | 58.7 | 118.1 | 5.93 | 4.51 | 6.01 |
| r17-4-loan64-B | 40.47 | 57.1 | 113.2 | 5.39 | 4.33 | 5.82 |
| r17-5-loan64-A | 40.57 | 60.6 | 113.3 | 5.94 | 4.55 | 6.06 |
| r17-6-loan64-B | 40.57 | 59.2 | 113.3 | 5.42 | 4.36 | 5.86 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down.

- **Hand-offs:** 6.0 to 5.4 enqueues a WRITE, 4.56 to 4.34 wakes, 6.06 to 5.83 context switches, with no overlap.
- **nfsd:** 115.5 to 113.2 µs a WRITE (-2.2, -2%). The B runs agree within 0.1; the A runs spread over 5 µs, so the difference is about 1.5 standard errors.
- **The rings:** 59.6 against 58.2, inside the scatter.
- **The stall watched since round 16:** none. No WRITE took over 131 ms, and the server retransmitted 4 segments in r17-1 and none in the rest.

**The profile** (fl14 against fl17, µs a WRITE on nfsd; both totals 118):

| frame | round 14 | round 17 |
|---|---|---|
| `svc_tcp_recvfrom` | 40.25 | 39.46 |
| `svc_xprt_received` | 2.05 | 1.54 |
| `svc_thread_wait_for_work` | 3.66 | 3.42 |
| `svc_pool_wake_idle_thread` | 2.99 | 2.79 |
| `svc_tcp_restore_pages` | 1.52 | 1.34 |
| `schedule` | 4.34 | 4.13 |

**The wakes, traced again** ([wakesrc.txt](wakesrc.txt), p8r17 against p8):
- Partial hand-backs with `XPT_DATA` set: 0.55 to 0.06 a WRITE. Their wake calls: 0.56 to 0.07.
- Passes through `svc_recv()`: 8.7 to 7.7. Dequeues: 4.12 to 3.61. Empty passes: 4.39 to 3.93. Sleeps: 2.63 to 2.48.
- `svc_thread_wake_next()` calls: 2.37 to 2.01.

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, round 17 | 0.424 ms | 0.502 | 0.578 | 0.437 |
| A1, round 14 | 0.428 | 0.502 | 0.569 | 0.438 |
| B2, round 17 | 0.432 | 0.514 | 0.618 | 0.444 |
| A2, round 14 | 0.428 | 0.502 | 0.569 | 0.437 |

- No difference.
- At 4 in flight the hand-offs fall further than at saturation: 7.4 to 6.3 enqueues a WRITE, 7.2 to 6.3 wakes, 10.3 to 9.3 context switches (`pool_stats` around the untraced runs).

tardis2 runs round 17's sunrpc with round 13's nfsd from here on.

## What the wake path still has

- **Wakes that find nothing:** 0.55 a WRITE woken from sleep, plus some woken before they slept, each about 2 µs between the waker and the woken thread.
  - Removing them would need an enqueue to skip its wake while a running thread is about to look for work: a count of such threads, kept so that the last one to stop looking checks the queue again.
  - That saves only the wakes made while a thread is already looking. A thread that starts looking after the wake was sent still takes the work from the woken thread.
  - Worth 2 µs a WRITE at most, so not tried.
- **The passes themselves:** a 1 MiB record still takes about 4 reads, because each waits for half the open window, and the window is about 350 KB.
  - The window is set by `svc_tcp_rx_rcvbuf`, 750 KiB on one port since 22 October (Mike).
  - Round 10b went from 12 passes to 5.4 for 26 µs a WRITE, about 4 µs a pass. So a larger buffer would cut more than any of these.
  - It is Mike's setting: it was chosen for copy mode, where 1 MiB dropped frames at the card's FIFO.
