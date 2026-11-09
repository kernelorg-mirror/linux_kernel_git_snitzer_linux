# Round 83: the wake-to-run latency with staying off and on

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, round 82's stack
(`$L/svcstack-ST`, `kernel-7.3/canary.ST`).

Round 82 found that staying with a loaned record costs 2.3 GB/s at the
same cycles a WRITE.  The suggested reason is that every piece then waits
for a sleeping thread to wake on an idle core, where today about a
quarter of them go to a thread already running.  This round measures that
latency.

`wake-run.bt` measures two things:

- **`@piece_us`**: from the `svc_data_ready()` that releases an
  incomplete loaned record to the next `tcp_read_sock()` on that socket,
  whoever reads it.  The release is the call that finds `sk_rx_want` set
  and leaves `XPT_DATA` (bit 3) set.
- **`@wake_idle_us` and `@wake_busy_us`**: for every wakeup of an nfsd
  thread, from `sched_waking` to its switch-in, split by whether the
  target CPU was running the idle task at the wakeup.

`run345.sh` swaps the stack in once.  It then runs A (`svc_tcp_rx_stay_us=0`,
`t2-r345`) and B (1000, `t2-r346`), each with the trace for five seconds in
the window, and puts the stock stack back at the end.
