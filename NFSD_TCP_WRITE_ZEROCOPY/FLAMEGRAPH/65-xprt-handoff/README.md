# Round 65: the transport queue's line, and the handoff to the claimed thread

**The changes (Mike, 7 November),** from round 63's options, at the
canary's tip:

- **S**, `4d81ec261bf0` "SUNRPC: Give a pool's transport queue a cache
  line of its own": `sp_xprts` alone on `struct svc_pool`'s first line,
  the idle list and `sp_flags` at the start of the next, the struct still
  256 bytes.  `struct lwq` itself (its lock, `ready` and `new` on one line)
  cannot change on this boot: `__lwq_dequeue` is vmlinux's.
- **H**, `f9143b0ab42c` "SUNRPC: Hand an enqueued transport to the idle
  thread claimed for it": `svc_pool_enqueue_xprt()` claims the first idle
  thread with `RQ_WAKING` and a new `RQ_HANDOFF` in one `cmpxchg` and gives it
  the transport in `rq_handoff`, so the thread does not dequeue it.  Every
  way a thread can leave a handed transport behind queues it again (claimed
  but not first on the idle list, stopping, `svc_exit_thread()`).

## The runs ([run241.sh](run241.sh), [run241.out](run241.out))

Three whole server stacks built here by `build-svcstack.sh` from build 514's
source with round 64's flag change (B), and B with S or H (worktrees
`/dev/shm/wt-flag`, `wt-S`, `wt-H`): both changes alter types other modules
see, and S and H give 277 and 273 of sunrpc's 439 exports new versions.
Swapped in by `swap-sunrpc.sh` in turn, B S H B S H (`t2-r241..r246`), each
run a loan run on both ports at `twoport.sh`'s defaults after the settled
setting, a trim and 120 s of rest, sampled as `t2-r235`, the pool's counters
around it, the kernel's log after it.  Read by [read65.py](read65.py).  No
run had a new kernel warning, the stock stack was put back at the end with
nothing in `updates/`, and the audit found the settled setting.

Sampled cycles a WRITE on nfsd's CPUs, and pool_stats a WRITE over the run:

| run | | GB/s | nfsd | `_raw_spin_lock` | slow path | `__lwq_dequeue` | `svc_recv` | hand | enqueues | wakes |
|---|---|---|---|---|---|---|---|---|---|---|
| `t2-r241` | B | 70.50 | 245.1k | 8900 | 2206 | 412 | 4259 | | 7.89 | 4.88 |
| `t2-r244` | B | 70.58 | 245.4k | 9439 | 2883 | 478 | 4513 | | 8.01 | 4.91 |
| `t2-r242` | S | 70.20 | 244.2k | 9182 | 2533 | 1541 | 3137 | | 7.89 | 4.83 |
| `t2-r245` | S | 70.46 | 246.5k | 9410 | 2625 | 1618 | 3369 | | 8.00 | 4.83 |
| `t2-r243` | H | 70.38 | 248.4k | 8443 | 1487 | 298 | 3933 | 645 | 9.23 | 5.78 |
| `t2-r246` | H | 70.11 | 242.8k | 7970 | 1524 | 309 | 3756 | 619 | 9.13 | 5.81 |

nfsd on average: **B 245.2k, S 245.4k, H 245.6k cycles a WRITE.**  The rate
is the same throughout: memory bandwidth holds it.

- **S moves the cost, it does not remove it.**  In both runs `__lwq_dequeue`
  gains about 1.1k cycles a WRITE and `svc_recv` loses about 1.1k: with the
  queue on a line of its own, its misses land in the dequeue instead of in
  `svc_recv`'s reads of the pool, which the queue's line used to bring.
  nfsd is unchanged.
- **H takes the lock's cost down, and pays it back in wakes.**  The lock's
  fast and slow paths together are about 2k cycles a WRITE lower and
  `svc_recv` 0.5k, against 0.63k for `svc_pool_hand_idle_thread`.  But
  each WRITE now has 0.9 more thread wakes (5.8 against 4.9) and 1.2 more
  enqueues counted.  A handed transport goes to the idle thread, which is
  asleep, where before a thread just finishing a call often found it on
  the queue without sleeping.  The extra wakes and context switches use
  what the lock saved, and nfsd is unchanged.  The enqueues counted include
  the transports passed on by a claimed thread that could not leave the
  idle list, which count again.

## Kept, and chased further (Mike, 7 November)

Neither change saves nfsd's cycles as it stands, and neither is dropped:
each change is kept while it furthers the search, until it is shown
untenable.  So the next step is where their cycles moved, below.  H would
also need to hand off only where it saves a wake (to an idle thread still
running, or only when no thread finishing a call would find the queue
first).

## Where S's cycles went (Mike: keep chasing them)

`annotate62.py` with `ANN_FUNCS=sunrpc:svc_recv,vmlinux:__lwq_dequeue`
(`annotate-recv-r241.txt` to `-r245.txt`), cycles a WRITE, B against S:

| load | B | S | what it reads |
|---|---|---|---|
| `svc_pool_can_shrink()` in `svc_recv` | 1186, L2 9.2 | 77 | `sp_nrthreads`, `sp_nrthrmin` |
| `lwq_empty()` at the top of `__lwq_dequeue` | 29 | 1014, L2 4.8 | the queue's `ready` and `new` |

The miss moved.  On B the thread counts were on the queue's line, so
`svc_recv`'s check of whether the pool may shrink was the first load of it
and took the miss, and the dequeue found the line cached.  On S that check
hits, and the dequeue's first look at the queue takes the miss.  The line
is written by another CPU at every arrival and dequeue, so whichever load
comes first pays.  Two changes would remove or hide it, run in round 66:
S with H, where most transports skip the dequeue, and S with a read
prefetch of the queue at the top of `svc_recv()`.

Also in every run, about 1.2 to 1.5k each: `svc_xprt_get()`'s refcount after
the dequeue (the transport's line, last written on another CPU), and
`svc_thread_should_stop()`'s read of the thread's own `rq_flags`, which a
waker's `RQ_WAKING` claim writes from another CPU.
