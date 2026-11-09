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
