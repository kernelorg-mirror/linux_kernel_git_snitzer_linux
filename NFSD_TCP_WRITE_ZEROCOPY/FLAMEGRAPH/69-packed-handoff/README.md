# Round 69: the read-mostly fields back on the queue's line, with the handoff

**The variant (Mike, 7 November: "I'm wondering if having the
read-mostly members share the first cacheline with sp_xprts would avoid
structure bloat"):** **PH**, "SUNRPC: Pack struct svc_pool's read-mostly
fields with its queue" (`c8ceae10eb27`) with the handoff: `sp_xprts`,
the thread counts and `sp_rtc_rqsts` on line 0, the idle list,
`sp_flags` and `sp_all_threads` on line 1, then the counters; 256 bytes
with 40 of tail padding ([pahole-PH.txt](pahole-PH.txt)).  Against
round 66's **SH** (the queue alone on line 0, 40 bytes of hole).

## The runs ([run281.sh](run281.sh), [run281.out](run281.out))

SH PH SH PH SH PH (`t2-r281..r286`), each run as round 68's without the
probe.  No new kernel warning, the stock stack put back, the setting
audited.

| run | | GB/s | nfsd | `__lwq_dequeue` | `svc_recv` | far |
|---|---|---|---|---|---|---|
| `t2-r281` | SH | 70.05 | 243.1k | 857 | 2703 | 1.9% |
| `t2-r283` | SH | 71.27 | 249.1k | 1160 | 3116 | 1.4% |
| `t2-r285` | SH | 70.89 | 249.1k | 1061 | 3072 | 1.3% |
| `t2-r282` | PH | 70.79 | 242.3k | 296 | 3416 | 1.7% |
| `t2-r284` | PH | 69.87 | 241.7k | 280 | 3699 | 1.8% |
| `t2-r286` | PH | 70.45 | 248.3k | 340 | 4654 | 1.3% |

SH 70.74 GB/s and 247.1k cycles a WRITE on average, PH 70.37 and
244.1k: the same rate, PH about 1.2% fewer cycles, at the same size.

## Where the cycles went

`annotate62.py` with `ANN_FUNCS=sunrpc:svc_recv,vmlinux:__lwq_dequeue`
(`annotate-recv-r282.txt`, `-r283`, `-r285`, `-r286`), cycles a WRITE:

| load | SH | PH | what it reads |
|---|---|---|---|
| `lwq_empty()` in `__lwq_dequeue` | 800, L2 3.4 | 17 | the queue's line |
| `svc_pool_can_shrink()` before the wait | 63 | 1076, 1397 | `sp_nrthreads`, `sp_nrthrmin` |
| `svc_thread_should_stop()` | 1300, L2 9.1 | 715, 1425 | `sp_flags` |
| `svc_xprt_get()`'s refcount | 1178 | 1137, 1252 | the transport's line |

The queue line's miss moved back to the first read of that line, now
`svc_pool_can_shrink()`, as on B.  And in every layout
`svc_thread_should_stop()` costs as much again.  The disassembly says
why: at each wakeup `svc_recv()` clears `SP_TASK_PENDING` with an
`stclr` and `svc_thread_should_stop()` does `test_and_clear_bit()` of
`SP_NEED_VICTIM` with an `ldclral`, both unconditional atomics on
`sp_flags`, though neither bit is set outside `svc_wake_up()` and a
pool's shrinking.  With 512 threads waking 5.7 times a WRITE, every
wakeup writes the line.  Round 70 reads the bits first.
