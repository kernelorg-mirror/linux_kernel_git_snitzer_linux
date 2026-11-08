# Round 72: nfsd's loop reading only the thread's own flag

**The change (Mike, 8 November: "write the nfsd loop change on PG and
test it"):** round 71 found nfsd's loop, `while
(!svc_thread_should_stop(rqstp))`, paying 1.1k cycles a WRITE for its
load of `sp_flags` on the queue's line.  "SUNRPC: Add
svc_thread_chosen_to_stop()" (`fc5465db0a78`) reads only `RQ_VICTIM` in
the thread's own `rq_flags`, which `svc_recv()` sets through
`svc_thread_should_stop()` on every path that returns without a
request, and "NFSD: Check only the thread's own flag in nfsd()'s loop"
(`8f395d70e9c6`) uses it.  **PN** is round 71's PG with both
(worktree `/dev/shm/wt-PN`, branch `kernel-7.3/canary.PN-514`).

## The runs ([run299.sh](run299.sh), [run299.out](run299.out))

PG PN PG PN PG PN (`t2-r299..r304`), each run as round 71's.  No new
kernel warning, the stock stack put back, the setting audited.

| run | | GB/s | nfsd | `svc_recv` | far |
|---|---|---|---|---|---|
| `t2-r299` | PG | 71.14 | 247.4k | 2141 | 1.4% |
| `t2-r301` | PG | 70.75 | 243.4k | 2026 | 1.7% |
| `t2-r303` | PG | 69.97 | 240.2k | 2018 | 2.2% |
| `t2-r300` | PN | 70.88 | 245.0k | 3193 | 1.5% |
| `t2-r302` | PN | 71.55 | 249.6k | 3041 | 1.1% |
| `t2-r304` | PN | 70.45 | 245.7k | 3072 | 1.9% |

PG 70.62 GB/s and 243.6k cycles a WRITE on average, PN 70.96 and
246.8k, both on the line along which cycles rise with the rate.

## Where the cycles went

[symdiff.py](symdiff.py), PG's three runs against PN's three: nfsd's
`svc_thread_should_stop()` falls from 996 cycles a WRITE to 65, and
`svc_recv()` rises from 2062 to 3102.  The annotation
(`annotate-recv-r299.txt`, `-r300`, `-r304`) puts the rise on one load,
`svc_pool_can_shrink()` before the wait, which reads the thread counts
on the queue's line: 123 cycles in PG, 1171 and 1212 in PN.

The miss moved, as in rounds 65, 69 and 71: the queue's line is
written by other CPUs while a thread handles its request, and the
first read of it afterwards pays, whichever read that is.  The thread
has to read the line at least once a pass (it must look at the queue
before it sleeps), so moving reads off it no longer saves anything;
what is left is to have it written less, or to read it fewer times
than the two a pass it is read now (before the wait, and
`SP_TASK_PENDING` after it, 0.4k).
