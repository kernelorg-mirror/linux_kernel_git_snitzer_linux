# Round 119: the server's per-READ cost at 2.78 M, again

Mike, 18 November: "take a fresh profile at 2.78 M after r1005".

Round 113 (t2-r972, 2.54 M, before the file handle borrow and the spill)
put the server at about 18 us of CPU a READ: rtc work 11.3 (process 7.3,
of it proc_read 5.2: XFS read 3.0 and the fh lookup 1.9; send 3.6; recv
2.6) and softirq 6.1.  The fh borrow (round 114) should have taken most
of the 1.9, yet round 116's t2-r995 works out at 18.4 us a READ from its
busy (80% of 64 CPUs at 2.78 M).  This profile shows where the time
goes now, and what to cut next: the reply's send, softirq, the XFS read,
the receive or the rest of the processing.

    ../113-server-per-read/prof-srv.sh 1006

One run under `settled-iops.sh` as held at `28eb5f75dd587` (the spill at
weight 1, the three borrows, nconnect 12), 8 s of `perf record -a -g` at
999 Hz from the load's second 8.  A profile changes no path: no passes.
Queued after t2-r1005, the setting's confirmation run.
