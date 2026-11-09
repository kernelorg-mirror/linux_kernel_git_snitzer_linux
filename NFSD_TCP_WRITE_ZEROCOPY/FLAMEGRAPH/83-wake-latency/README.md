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

## Result: a piece is read sooner with staying, so latency is not the cause

`run345.out`; the histograms are in `wake-run-A.txt` and `wake-run-B.txt`.
Neither run raised a new kernel warning, and the stock stack is back.

| | A, off (`t2-r345`) | B, on (`t2-r346`) |
|---|---|---|
| rate, with the trace on | 70.99 GB/s | 68.43 GB/s |
| pieces timed in 5 s | 1,996,890 | 2,056,230 |
| release to read of a piece, mean | 7.79 us | **3.40 us** |
| the same, its bulk | 4 to 16 us | 2 to 8 us |
| wakeup to run, target CPU idle, mean | 4.47 us | 4.68 us |
| wakeup to run, target CPU busy, mean | 5.99 us | 6.18 us |
| nfsd wakeups a second | 275k | 496k |
| nfsd wakeups a WRITE | 4.05 | 7.59 |
| pieces a WRITE | 5.88 | **6.29** |

- **Round 82's explanation is wrong.**
  - A staying thread reads the next piece in less than half the time the
    pool takes.
  - Waking from an idle core costs the same 4.5 to 4.7 us either way.
  - So the 2.3 GB/s is not lost in the wait for a piece.
- **What does differ.**
  - B wakes nfsd threads 1.9 times as often a WRITE, since every piece is
    now a real sleep and wake.
  - B takes a record in 7% more pieces.  This fits a read that starts
    sooner and finds less queued beyond what `sk_rx_want` waited for,
    though that is a reading and not measured.
  - Neither shows up in cycles a WRITE (round 82: 242.3k against 243.4k).
- **Still unexplained.**  A WRITE's time from its last piece to its reply,
  and the client's view of a WRITE's round trip, have not been looked at.
  The 128 WRITEs in flight make the rate their latency's inverse, so the
  3% is somewhere in it.  The drives are equally busy in A and B (85 to
  93%; round 82's `results.tsv`).

(As in round 81, the `@armed` and `@cur` maps at the end of each trace are
bookkeeping that filled again between the `clear()` and the exit.)
