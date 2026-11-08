# Round 73: a handed transport's reference taken by the enqueuer

**The change (Mike, 8 November: "write the refcount change on PN and
test it"):** rounds 69 to 72 found `svc_recv()` paying about 1.2k
cycles a WRITE on the refcount increment of `svc_xprt_get()`, whose
`xpt_ref` shares a line with `xpt_flags`, which the enqueuer has just
written (`XPT_BUSY`).  "SUNRPC: Take a handed transport's reference in
the enqueuer" (`51cbfc0196e3`) takes the reference in
`svc_pool_hand_idle_thread()`, before the transport is stored for the
thread, and the paths that queue a handed transport again drop it.
**PR** is round 72's PN with it (worktree `/dev/shm/wt-PR`, branch
`kernel-7.3/canary.PR-514`).

## The runs ([run305.sh](run305.sh), [run305.out](run305.out))

PN PR PN PR PN PR (`t2-r305..r310`), each run as round 72's.  No new
kernel warning, the stock stack put back, the setting audited.

| run | | GB/s | nfsd | `svc_recv` | far (a, b) |
|---|---|---|---|---|---|
| `t2-r305` | PN | 69.46 | 239.3k | 2879 | 3.1%, 3.3% |
| `t2-r307` | PN | 70.45 | 242.7k | 3374 | 1.1%, 1.3% |
| `t2-r309` | PN | 70.25 | 241.6k | 3092 | 2.0%, 1.7% |
| `t2-r306` | PR | 70.50 | 245.0k | 3140 | 2.3%, 1.9% |
| `t2-r308` | PR | 71.27 | 250.3k | 3569 | 1.4%, 0.5% |
| `t2-r310` | PR | 70.71 | 246.0k | 2892 | 1.4%, 2.3% |

PN 70.05 GB/s and 241.2k cycles a WRITE on average, PR 70.83 and
247.1k: PR is ahead in rate in all three pairs, and its cycles rise
with the rate as before.  At matched rates (`t2-r307` against
`t2-r306`, 70.45 and 70.50) PR takes 2.3k more cycles a WRITE, so the
rate is not the change's doing.

## Where the cycles went

The annotation (`annotate-recv-r306.txt` for PR, `-r309.txt` for PN)
still has the refcount miss in `svc_recv()` in PR, 1115 cycles a WRITE
against 1195, at the increment of the `svc_xprt_get()` in
`svc_xprt_dequeue()`: the transports that pay it are the ones taken
off the queue, not the handed ones.  `pool_stats` says why: about 9.1
enqueues a WRITE and 5.8 threads woken, and the handoff gives a
transport only to the first idle thread on the list, so most
transports are queued and dequeued.  The handed path's increment in
`svc_pool_hand_idle_thread()` costs about 40 cycles a WRITE.

So the change does what it says for the transports it covers, and
those are too few to show.

## Rounds 66 to 73, closed

This line of work stops here (Mike, 8 November: "we need to stop going
down the rabbit hole on this though, please try to wrap up this line
of exploration/work").  What it leaves at the tip of
`kernel-7.3/canary`, none of it pushed:

- `7a8f1f776f25`, `c8ceae10eb27`: `struct svc_pool` laid out by who
  writes each line, its read-mostly fields packed with the queue
  (rounds 67 and 69; PH took about 1.2% fewer cycles a WRITE than SH
  at the same rate).
- `a14d5ab8953e`: the pool's flags read before they are cleared on
  each wakeup (round 70; about 1.1k fewer cycles a WRITE, 0.4%, at
  matched rates).
- `74c1391ce42e`: the flags on the line with the read-mostly fields
  (round 71; the miss moved, no net change).
- `fc5465db0a78`, `8f395d70e9c6`: nfsd's loop reading only the
  thread's own flag (round 72; the miss moved, no net change).
- `51cbfc0196e3`: the handed transport's reference (this round; no
  net change).

What was learned: the queue's line (`struct svc_pool`'s line 0) is
written by other CPUs on every enqueue and dequeue, often enough that
two back-to-back reads of it both miss.  A thread has to read it at
least once a pass, so moving reads off it only moves the miss (rounds
65, 69, 71, 72).  The transport's refcount line is the same: the
enqueuer has it, and the thread that dequeues pays for it.  What is
left, if this is taken up again, is to have the queue written less (more
of the transports handed, fewer queued), not to read it differently.
