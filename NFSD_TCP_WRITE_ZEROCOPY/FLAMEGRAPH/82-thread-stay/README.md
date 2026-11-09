# Round 82: a thread stays with a loaned record for its rest

9 November, tardis2, canary `7.3.3-13.hs.519.canary` (built from
`3bdeea5a21cc`).

Round 81 found that a woken nfsd thread runs again on the CPU it slept on
99% of the time, and put what a parked piece costs at up to about 23k
cycles a WRITE.  This round builds the change and A/Bs it.

## The change

`kernel-7.3/canary.ST` (local only, worktree `/dev/shm/wt-ST`), one code
commit on the canary's tip: "SUNRPC: Let a thread stay with a loaned
record for its rest" (`02a9b93793f7`).

- `svc_tcp_rx_stay()` runs in `svc_tcp_recvfrom()` where a loaned record
  is left incomplete with `sk_rx_want` set.  The thread keeps `XPT_BUSY`
  and the record's state, publishes itself in the new
  `svsk->sk_rx_waiter`, and sleeps interruptibly.
- `svc_data_ready()`, once it sets `XPT_DATA` for the record, wakes the
  waiter in place of `svc_xprt_enqueue()`.  The thread clears `XPT_DATA`
  and reads on from `again:`.
- The wait ends without data on:
  - the timeout, `svc_tcp_rx_stay_us` rounded up to the tick and one more
    (`HZ=100`, so 1000 us is 10 to 20 ms);
  - `XPT_CLOSE`, which `svc_tcp_state_change()` now also wakes the waiter
    for;
  - `SP_NEED_VICTIM` on the pool;
  - `kthread_should_stop()`;
  - a pending signal.

  The record is then saved as before, and `svc_xprt_received()` queues
  the transport again if data came meanwhile.
- The waiter is published before the task state is set, and that barrier
  orders it before the `XPT_DATA` test.  `svc_data_ready()` reads the
  waiter after its `test_and_set_bit()`.  So either the wait sees the
  data or the waker sees the waiter.
- `svc_tcp_rx_stay_us` defaults to 0, which is off.
  `svc_tcp_rx_stay_stats` (read only) counts the waits begun, those that
  ended with data and those that ended without.

The new field in `struct svc_sock` changes the versions of 273 of
sunrpc's exports, because gendwarfksyms takes them from the types'
layout.  So the whole server stack is built with `build-svcstack.sh` into
`$L/svcstack-ST` and swapped in once.

## The A/B

`run337.sh`: A is `svc_tcp_rx_stay_us=0`, B is 1000, on the same modules.
The runs are A B A B A B with perf as in round 78 (`t2-r337..r342`), then
A and B with the held mark for the region's exact peak (`t2-r343`,
`t2-r344`).  At the end the stock stack is put back.

## Result: staying costs 2.3 GB/s at the same cycles a WRITE

`run337.out`.  Every run had no new kernel warning, and the stock stack
went back in at the end (sunrpc `2A2297D3850E37768921BCC`, `updates/`
empty, the settled setting).

| run | | GB/s | threads woken a second | context switches a second |
|---|---|---|---|---|
| `t2-r337` | A | 71.32 | 289,818 | 810,578 |
| `t2-r338` | B | 69.33 | 84,793 | 1,079,386 |
| `t2-r339` | A | 71.24 | 290,955 | 812,421 |
| `t2-r340` | B | 68.01 | 82,981 | 1,048,706 |
| `t2-r341` | A | 69.59 | 292,042 | 822,399 |
| `t2-r342` | B | 67.97 | 83,442 | 1,053,714 |
| `t2-r343` | A, held mark | 70.58 | 292,423 | 811,958 |
| `t2-r344` | B, held mark | 68.74 | 84,093 | 1,064,034 |

- **Rate.** A's mean is 70.72 GB/s and B's 68.44: B is lower in every
  pair, by 2.3 GB/s (3.2%).  The held-mark pair agrees (70.58 against
  68.74).
- **The change works as meant.**
  - Each B run began about 22 million waits, and at most 741 of them
    ended without data.
  - Threads woken through the pool fell by 71%.
  - nfsd was 13 to 15% busy in both.
- **Cycles a WRITE are the same** (`symdiff-AB.txt`, the three A runs
  against the three B): 243.4k against 242.3k.
  - What round 81 priced went away, about 12.7k among the top movers:
    - `svc_recv` -1,954;
    - the spin locks -2,668;
    - `__rx_loan_table_append_ref` -1,050, the takeover misses, which
      round 80 blamed;
    - `svc_xprt_recv_call` -972;
    - `svc_pool_wake_idle_thread` -946;
    - `lock_sock_nested` -840;
    - `svc_xprt_received` -828;
    - `svc_tcp_restore_pages` -825;
    - `svc_thread_should_sleep` -823;
    - `svc_tcp_rx_validate_mixed` -802.
  - A sleep and a wake on every piece came in its place, about 12.2k:
    - `finish_task_switch` +7,092;
    - `svc_tcp_rx_stay` itself +2,553;
    - the idle entry and the rest.
  - Context switches rose 30%, about 250k more a second.
- **The region.** The exact peak was 11,738 pages for A and 9,907 for B,
  so staying raised it in neither run.  A's figure is above round 77's
  8,092 on the stock modules.  One run each says nothing about that gap.

So the change trades the trip through the pool for a sleep and a wake at
the same cost.  The cost it adds is in latency, since nfsd is mostly
idle.  Today about a quarter of the pieces go to a thread that is already
running (some 400k pieces a second against 290k wakes).  A staying thread
is always asleep when the rest arrives, on a core that has gone idle, so
every piece waits for a full wake and exit from idle.  With a window of
512 KiB, each piece's delay holds back the connection.  This explanation
is not measured: a trace of the wake-to-run time with staying off and on
would test it.

`finish_task_switch` is also where samples land that the interrupts held
off during the switch would have taken (no pseudo-NMI on this boot), so
its +7k is not all switching.

**Not kept as it is.**  The branch `kernel-7.3/canary.ST` and the stack
`$L/svcstack-ST` are kept for a variant.  One variant would spin briefly
(a few microseconds) before sleeping, or let the waker hand the piece to
a running pool thread when the waiter's core is idle; either would keep
the latency of today's handoff.  Note that a busy-wait (patch 0005) cost
about 3 of the 48 CPUs in October.
