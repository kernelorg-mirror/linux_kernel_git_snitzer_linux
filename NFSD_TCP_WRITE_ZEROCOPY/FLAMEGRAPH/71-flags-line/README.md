# Round 71: the pool's flags on the queue's line

**The change (Mike, 8 November: "move sp_flags next to the read-mostly
fields and test it"):** "SUNRPC: Keep a pool's flags on the line with
its read-mostly fields" (`74c1391ce42e`).  With round 70's change a
wakeup only reads `sp_flags`, so it moves to line 0 beside the thread
counts, and `sp_rtc_rqsts` moves to line 1 with the idle list and
`sp_all_threads`.  Still 256 bytes, line 0 with 16 bytes free
([pahole-PG.txt](pahole-PG.txt)).  **PG** is round 70's PF with it
(worktree `/dev/shm/wt-PG`, branch `kernel-7.3/canary.PG-514`).

## The runs ([run293.sh](run293.sh), [run293.out](run293.out))

PF PG PF PG PF PG (`t2-r293..r298`), each run as round 70's, with
`pmu.script` kept xz-compressed, as root had 1.6 GB free.  No new kernel
warning, the stock stack put back, the setting audited.

| run | | GB/s | nfsd | `svc_recv` | far |
|---|---|---|---|---|---|
| `t2-r293` | PF | 69.94 | 238.1k | 2153 | 1.8% |
| `t2-r295` | PF | 70.28 | 243.2k | 2268 | 2.5% |
| `t2-r297` | PF | 70.91 | 246.6k | 2803 | 1.8% |
| `t2-r294` | PG | 71.70 | 249.1k | 2064 | 1.3% |
| `t2-r296` | PG | 71.18 | 248.7k | 2064 | 1.1% |
| `t2-r298` | PG | 70.70 | 246.1k | 2154 | 1.5% |

PF 70.38 GB/s on average, PG 71.19: PG ahead in every pair, at lower
far shares in every pair too, so not yet a gain of its own.

## Where the cycles went

[symdiff.py](symdiff.py) `t2-r297 t2-r298`, the nearest pair in rate
(70.91 and 70.70), cycles a WRITE: 246.6k against 246.1k.

| function | PF | PG |
|---|---|---|
| `svc_recv` | 2803 | 2154 |
| `svc_rtc_thread_run_from` | 540 | 304 |
| `svc_thread_wait_for_work` | 1103 | 964 |
| `svc_thread_should_stop` | 528 | 1184 |
| `svc_pool_idle_add` | 571 | 807 |
| `llist_del_first_this` | 940 | 1047 |

The miss moved again.  `svc_recv()` now finds the flags on the line its
thread-count check has just brought in, and saves about 0.65k.  But
`svc_thread_should_stop()` gains as much: the copy that costs it is
nfsd's own, out of line in `nfsd.ko` since round 70 and called once a
request by nfsd's loop, `while (!svc_thread_should_stop(rqstp))`.
Its hot instruction (1.1k cycles a WRITE) is the load of `sp_flags`,
now on the queue's line, which other CPUs write while the thread
handles its request.  sunrpc's copy, called in `svc_recv()` after the
wait, costs 70 to 90 (`annotate-recv-r29[678].txt`).

That check is a repeat: every `svc_recv()` checks `SP_NEED_VICTIM`
after its wait and sets `RQ_VICTIM` in the thread's own `rq_flags`, so
nfsd's loop needs only to read `RQ_VICTIM` to see a thread chosen to
stop.
