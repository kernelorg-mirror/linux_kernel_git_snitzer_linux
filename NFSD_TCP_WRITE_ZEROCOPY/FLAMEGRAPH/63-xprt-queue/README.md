# Round 63: the thread pool's transport queue, a WRITE

**The question (Mike, 7 November):** round 62 found lock contention up
6.9k cycles a WRITE against 506, and fl64 put most of the lock's slow path
in `svc_recv`'s `__lwq_dequeue` (0.86 of 1.31 us a WRITE): 512 threads in
one pool taking transports off one queue, `pool->sp_xprts`.  What makes it
contended, and what to do about it?

## The run ([run236.sh](run236.sh), [run236.out](run236.out))

Build 514's held-mark kernel, both ports, budget 128, the client at
`twoport.sh`'s defaults, after a trim of the eight and 120 s of rest:
**t2-r236**, 68.72 GB/s with [deq.bt](deq.bt) for 9.5 s inside the window
(581,065 WRITEs), output in `$L/t2-r236/deq.txt` ([deq-r236.txt](deq-r236.txt) without the warnings).  sunrpc's functions are
kprobes (its BTF does not match this boot's vmlinux); the lock's slow path
is fentry and fexit.  `__lwq_dequeue` cannot be probed at all (not
traceable), so the queue's lock is told apart by its address: the pool from
`svc_pool_enqueue_xprt()`, `sp_xprts` at +16 with its lock first
(`struct svc_pool`, 514's `sunrpc.ko.debug`).  The probe on the slow path
lengthens the waits it times: read the counts, and the times only as an
order.

## A WRITE goes round the queue five and a half times

| a WRITE | |
|---|---|
| `svc_xprt_enqueue()` calls, all from `tcp_data_ready()` | 5.58 |
| of them put on the queue (the transport not already busy) | 5.56 |
| `svc_xprt_received()`, a thread done with the transport | 5.31 |
| a thread claimed for it (`svc_pool_wake_idle_thread()` true) | 2.95 |
| none claimed: the first idle thread already being woken | 5.68 |
| the queue's lock taken through the slow path | **1.23** |
| that wait, probed | about 4.1 us |
| other locks through the slow path (fl64: mostly `lock_sock_nested`) | 1.80 |

A 1 MiB record arrives in pieces, and for each piece that finds the
transport idle `tcp_data_ready()` queues it again: a thread is woken or a
running one picks it up, takes the queue's lock to dequeue it, reads what
has come, finds the record incomplete, hands the partial record back to the
socket (`svc_tcp_save_pages()`) and releases the transport; the next piece
starts it over on another thread (`svc_tcp_restore_pages()`).  So the queue
is passed about 5.5 times for each WRITE, and one pass in 4.5 finds its lock
held.  The lock is not held long (`__lwq_dequeue` is a few loads): it is
contended because 48 CPUs pass the same line about 350k times a second.  The line
also holds the queue's `new` list, which every enqueue's `llist_add()`
writes, and `sp_rtc_rqsts`'s lock (`struct svc_pool` at +0 to +63).

**The passes are the multiplier, and they cost more than the lock.**  Each
one is a wake or a pickup, a dequeue under the lock, a save and a restore of
the partial record's pages (each writing `svc_tcp_spare_pages`, the global
atomic whose line round 62 found false-shared with the KUnit fault hook's
flag: 1.76k cycles a WRITE in `svc_tcp_rx_append_ref`), and a release.

## What to do, by how much it would take off

1. **Fewer passes a WRITE.**  Receiving more of the record each pass cuts
   the lock, the wakes, the saves and restores and the spare counter's
   traffic together.  The levers tried before were judged on the rate, on a
   rig whose rate is set by memory bandwidth: the SO_RCVLOWAT mark
   (`svc_tcp_rx_lowat`, dropped 15 October) and the linger
   (`svc_tcp_rx_linger_us`, dropped after build 474's A/B).  Measured by
   cycles a WRITE instead, one of them could read differently.  That is
   Mike's to decide; nothing here brings them back.
2. **Hand the transport to the thread claimed for it.**  When
   `svc_pool_enqueue_xprt()` claims an idle thread, give it the transport in
   its `svc_rqst` and leave the queue to the case with none idle: the 2.95
   claimed passes a WRITE would take no lock.  Ordering against
   `svc_thread_wake_next()` and the steals needs care.
3. **Split `struct svc_pool`'s first line:** `sp_xprts`'s lock and `new`
   apart from each other and from `sp_rtc_rqsts`, so that enqueues do not take
   the line from the dequeuer.  Small, and the slow-path count would show
   whether it helps.
4. **The spare counter** (from round 62): the KUnit flag behind a static
   key or off the counter's line; the counter per socket or per CPU.  It
   is paid on every pass.

Several pools a node is not on the list: Chuck has called it a dead end
and it was dropped on 15 October.  This canary's `svc_recv` is far from
upstream's (sibling pools, steals, the idle spin, run-to-completion work
items), so a change here may not carry to the main series as it is.
