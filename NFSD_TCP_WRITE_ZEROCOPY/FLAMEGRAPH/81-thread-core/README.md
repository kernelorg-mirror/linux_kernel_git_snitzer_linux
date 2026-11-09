# Round 81: would keeping a record's pieces on one thread keep them on one core?

9 November, tardis2, canary `7.3.3-13.hs.519.canary` (built from
`3bdeea5a21cc`), stock sunrpc, the settled setting.

Round 80 found that a loaned record arrives in 5.9 pieces and that 93% of
them run on another core than the piece before.  Each piece goes to
whichever thread is on top of the pool's idle list (last in, first out, 512
threads over CPUs 8-55), so it lands on a different thread almost every
time.  The change considered is for the thread that read a piece to keep
the record and sleep until `svc_data_ready()` finds the rest has arrived,
then wake that thread directly.  That is the sleeping form of the linger
dropped in October (`patches/dropped/0005`), which busy-waited instead.
It rests on two things, checked here before any code is written:

1. a thread that sleeps and is woken runs again on the core it slept on,
   so keeping the thread keeps the core;
2. what the parked pieces cost today is worth the change.

## 1. A woken nfsd thread runs on its last CPU (`t2-r336`)

`wake-cpu.bt` ran for five seconds in a two-port loan run (70.38 GB/s, no
new kernel warnings; the trace costs the run its rate, so the figure is not
one to compare).  At each switch-in of an nfsd thread it compares the CPU
with the one the thread switched out on, by how long the thread was off.

| | same CPU | another CPU | same |
|---|---|---|---|
| every switch-in | 1,735,054 | 14,434 | **99.2%** |
| off 10 us to 1 ms (a piece's gap is 32-64 us) | 869,421 | 12,691 | **98.6%** |

Of the 14,434 moves, the old CPU was running something else at the time in
11,951 and was idle in 2,483.  So the scheduler keeps a woken nfsd thread
where it was, and moves it mainly when that core is taken.  Round 80's 93%
comes from pieces going to different threads, not from threads moving.

(The `@cur`, `@out` and `@outt` maps at the end of `wake-cpu.txt` are
bookkeeping that filled again between the `clear()` and the exit.  They
mean nothing.)

## 2. What a parked piece costs today (`t2-r327`)

`price.py` reads round 77's call graph (`t2-r327`, 69.61 GB/s, cycles a
WRITE) in one pass: `price-r327.txt`.  The first column is every cycle
under the function.  The second leaves out cycles also under
`svc_tcp_recvfrom`, `schedule_timeout` or `svc_process`.

| | cycles a WRITE |
|---|---|
| `svc_recv`, outside the receive, the sleep and the procedure | 21,376 |
| of it: `svc_thread_wake_next` | 4,831 |
| of it: `svc_thread_wait_for_work`, outside the sleep | 4,200 |
| of it: `svc_alloc_arg` | 2,029 |
| of it: `svc_xprt_release` (once a record, stays) | 1,332 |
| `svc_xprt_received` (in the receive) | 3,167 |
| `svc_tcp_restore_pages` + `svc_tcp_save_pages` | 1,095 |
| `schedule_timeout` (the sleep and wake itself, stays) | 6,666 |
| `svc_data_ready` (ring CPUs) | 798 |

The thread loop runs once a piece: 5.92 a WRITE, of which 4.92 end with
the record parked.  Under the change those 4.92 skip the loop and the
received, save and restore, and keep only the sleep and the wake.  The
upper bound is:

- the loop's per-piece cycles, (21,376 - 1,332) x 4.92/5.92: about 16,700;
- `svc_xprt_received`, 3,167 x 4.92/5.92: about 2,600;
- the save and restore: about 1,100;
- the loan table's takeover misses (round 79): about 2,900, gone when the
  pieces stay on one core.

That is about **23,000 cycles a WRITE, some 10% of the 237,000** nfsd
cycles a WRITE (round 76).  It is an upper bound:

- perf skids each sample without SPE;
- a parked thread still pays its wake from the ring CPU;
- a thread is held per partially received record, at most one a
  connection (24 of 512);
- the October keep (0006) cut wakeups by a quarter with no change in
  rate.

## Where it leaves the change

Both checks pass: keeping the thread keeps the core, and the cycles it
would save are many times the prefetches' (round 79's 2.9k is their whole
target).  The design as sketched:

- at `err_incomplete` in `svc_tcp_recvfrom()`, a loaned record with
  `sk_rx_want` set is neither saved nor released: the thread keeps
  `XPT_BUSY` and its `rq_tcp_rx`, sets `svsk->sk_rx_waiter`, and sleeps
  interruptibly with a timeout of about 1 ms;
- `svc_data_ready()`, once `svc_tcp_rx_wait_rest()` no longer holds the
  record back, wakes the waiter in place of the enqueue, and the thread
  reads on from `again:`;
- the timeout, a close or `sk_state_change`, a thread stopping, a region
  credit wait, a control record and TLS each park the record as today's
  code does.  Each must clear the waiter in the same order `XPT_DATA` is
  handled now; that is where the bugs would be.

The other ways were turned down.  Waking the previous thread through the
pool would need a new idle structure, because the idle list lets only its
top thread be claimed.  `xpt_home_cpu` picks a pool, not a core.  Taking
the whole record in one piece needs a window as large as the record,
which would grow the region.

It is a sunrpc-only change, so it can be A/B'd by swapping `sunrpc.ko`
behind a switch, as rounds 55 and 58 were.
