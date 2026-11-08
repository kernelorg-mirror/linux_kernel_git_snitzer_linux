# Round 70: the pool's flags read before they are cleared

**The change (Mike, 8 November: "write the sp_flags change on P+H and
test it"):** round 69 found two unconditional atomics on `sp_flags` at
every wakeup.  "SUNRPC: Read a pool's flags before clearing them on
each wakeup" (`a14d5ab8953e`) tests `SP_TASK_PENDING` before
`svc_recv()` clears it, and `SP_NEED_VICTIM` before
`svc_thread_should_stop()`'s `test_and_clear_bit()`.  **PF** is round
69's PH with it (worktree `/dev/shm/wt-PF`, branch
`kernel-7.3/canary.PF-514`); `svc_recv()` now reads `sp_flags` with an
`ldr` and branches past the `stclr`, and the `ldclral` runs only when
the bit is set.  `svc_thread_should_stop()` is no longer inlined: sunrpc
and nfsd each have an out-of-line copy.

## The runs ([run287.sh](run287.sh), [run287.out](run287.out))

PH PF PH PF PH PF (`t2-r287..r292`), each run as round 69's.  No new
kernel warning, the stock stack put back, the setting audited.

| run | | GB/s | nfsd | `svc_recv` | `__lwq_dequeue` | wakes | far |
|---|---|---|---|---|---|---|---|
| `t2-r287` | PH | 69.57 | 236.9k | 3141 | 234 | 5.88 | 1.9% |
| `t2-r289` | PH | 71.27 | 246.5k | 3991 | 320 | 5.66 | 1.2% |
| `t2-r291` | PH | 71.36 | 248.0k | 3999 | 282 | 5.60 | 1.2% |
| `t2-r288` | PF | 71.33 | 248.3k | 2758 | 389 | 5.65 | 1.3% |
| `t2-r290` | PF | 70.48 | 242.5k | 2691 | 396 | 5.80 | 1.9% |
| `t2-r292` | PF | 70.93 | 244.2k | 2597 | 329 | 5.73 | 1.7% |

PH 70.73 GB/s and PF 70.91 on average: the same.  nfsd's cycles a
WRITE rise with the rate on this rig (`t2-r287`, the slowest run, has
the fewest), so the three runs at 71.3 GB/s are the ones to compare.

## Where the cycles went

[symdiff.py](symdiff.py) `t2-r289,t2-r291 t2-r288,t2-r292`, cycles a
WRITE, PH's two runs at 71.3 GB/s against PF's two nearest (247.3k
against 246.2k):

| function | PH | PF | |
|---|---|---|---|
| `svc_recv` | 3995 | 2678 | the `ldclral`'s 1.3k gone |
| `svc_thread_should_sleep` | 1987 | 1024 | its read of `sp_flags` now hits |
| `nfsd` + `svc_thread_should_stop` | 1697 | 677 | nfsd's loop checks it too |
| `llist_del_first_this` | 1618 | 1123 | the idle list, on `sp_flags`' line |
| `finish_task_switch` | 23048 | 23879 | |
| `_raw_spin_lock` + slow path | 10095 | 10823 | |
| `svc_pool_idle_add` | 353 | 623 | |

The miss is gone, not moved: about 3.8k cycles a WRITE fewer in the
pool's own functions (`annotate-recv-r28[89].txt`, `-r29[012].txt`:
`svc_thread_should_stop` 75 to 96 cycles in sunrpc, where its inlined
atomic cost 1.3k).  About 2.7k of it reappears elsewhere, the most in
`finish_task_switch` (0.8k), the spinlock (0.7k) and
`svc_pool_idle_add()` (0.3k), whose `llist_add()` to `sp_idle_threads`
most likely now finds line 1 no longer owned (the flags' atomics had
brought it in).  nfsd as a whole
is about 1.1k (0.4%) lower at the same rate.
