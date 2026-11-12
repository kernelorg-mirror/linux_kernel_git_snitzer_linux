# Round 99: the held split with the client splicing, and the window again

**The ask (Mike, 12 November):** "go ahead with the held split", then
"once 99 finishes use it to inform next steps and pursue them", and
"run the window sweep after 99": whether a faster client changes the
calculus of round 94's window of loaned WRITEs a drive.

## What is known before it

- Round 98 put the two-port rate at 72 GB/s against the client's copy
  of every payload byte into TCP.  With the client's sunrpc splicing a
  WRITE's pages (`patches/client/0002`, loaded 12 November 05:49),
  `t2-r534` to `r536` carry 75.08, 74.99 and 75.26 GB/s, the client's
  `__pi_memcpy` 0.06% of its CPUs (18.4%), its idle 56% (28%), and the
  server's memory at 140 to 142 GB/s, 1.86 to 1.89 bytes a payload byte
  (1.72 in `t2-r531`): about 96% of the 148, the wall now.
- Loans denied went from 2.4% of WRITEs to about 5% at the settled
  budget of 64.  A denied record is not copied: its transport stops
  reading and waits for credit (`rx_loan_admit()`).
- Round 94 split the held pages by stage (`heldsplit.bt`) and the
  WRITE's time between XFS and the drive (`writesplit.bt`), and built a
  window of loaned WRITEs in flight for each export
  (`svc_tcp_rx_target_window`): at 7 it cost 3.5% of the rate for 6
  MiB of the median and the peak from 11,845 pages to 3,908, and below
  7 it cost more, with no knee.  It is in the loaded stack (sunrpc
  `FC78BAE8A6C2B0A92A1366A`), at 0.

## The held split (t2-r537 to r539)

[run537.sh](run537.sh) is round 94's `run485.sh` with the settled
budget (`TWO_WRITES`, 64) in place of 128; [run537.out](run537.out).
Every run clean: 0 ring drops, 12/12 flows, no drive stepped down, no
new kernel warning.  Beside round 94's run at the same budget
(`t2-r492`, 71.41 GB/s):

| stage, MiB | t2-r492 | t2-r537 | t2-r538 | t2-r539 |
|---|---|---|---|---|
| rate, GB/s | 71.41 | 74.78 | 74.92 | 75.07 |
| rings, posted | 8.0 | 8.0 | 8.0 | 8.0 |
| sockets' queues | 1.5 | 1.6 | 1.6 | 1.6 |
| 1 assembly | 10.7 | 10.2 | 10.1 | 10.2 |
| 2 to WRITE | 1.0 | 1.0 | 1.1 | 1.1 |
| **3 in the WRITE** | **23.2** | **27.5** | **27.5** | **28.3** |
| 4 to release | 0.7 | 0.8 | 0.8 | 0.8 |
| the way back, unseen | 6.5 | 5.4 | 6.0 | 5.3 |
| loans denied | 2.4% | 4.8% | 4.4% | 5.1% |

The splice moved one stage: the WRITE's, from 23.2 to about 27.8 MiB,
half of all held.  At the rate, that is 0.34 ms a WRITE before and
0.39 ms now.  Assembly, the way back and the rest are where they were,
so neither a smaller `wsize` nor work on the way back is the lever.

## The WRITE's time (t2-r540)

`BT=writesplit`, one run, [run540.out](run540.out), 75.13 GB/s, clean.
Beside round 94's (`t2-r488`, `r489`, 71.4 GB/s):

| | t2-r488/r489 | t2-r540 |
|---|---|---|
| before the I/O (XFS, iomap) | 10 us | 10 us |
| **a request at the drive, issue to complete** | **335 to 341 us** | **394 us** |
| the rest of the I/O (blk-mq, the call's other requests) | 2.3 MiB | 1.8 MiB |
| after the last completion | 6 us | 6 us |
| the whole call | 410 to 417 us | 472 us |

All of the growth is the drive's.  A 256 KiB request transfers in
about 28 us at 9.4 GB/s, so 394 us is a queue of about 14 requests,
3.5 WRITEs, a drive (the inflight sampler's mean depth of 14 in
`t2-r534`), where round 94 found two keep a drive flat out.  The
drives are at 9.3 to 9.5 GB/s of their 10.2: near its capacity a
queue's wait grows faster than its rate, so 5% more rate cost 17% more
time at the drive.  That is the stage the window bounds.

## The window again (t2-r541 to r544)

Round 94's [run494.sh](../94-held-split/run494.sh) unchanged, with
`NOSWAP=1` (the stack has the window) and the budget at 128 as there,
so that the window and not the budget is the bound (round 98 found 64
and 128 at the same rate); [run541.out](run541.out).  Every run clean:
0 ring drops, 12/12 flows, no drive stepped down, no new kernel
warning, no loan denied.

| window | GB/s | held median (MiB, L3) | p99 | peak | in the WRITE | assembly | sockets | memory GB/s, bytes a byte | paused |
|---|---|---|---|---|---|---|---|---|---|
| 0 (t2-r541) | 75.12 | 3,963 (62, 1.94) | 7,255 | 11,040 | 30.0 | 10.7 | 1.6 | 141.0, 1.88 | 0 |
| 10 (t2-r542) | 75.14 | 3,869 (60, 1.89) | 4,407 | 5,097 | 24.5 | 9.7 | 1.9 | 138.5, 1.84 | 471 k |
| 8 (t2-r543) | 75.10 | 3,694 (58, 1.80) | 4,044 | 4,670 | 22.3 | 9.0 | 2.5 | 136.1, 1.82 | 1.23 M |
| **7 (t2-r544)** | **75.16** | **3,551 (55, 1.73)** | **3,820** | **4,364** | **21.2** | **8.5** | **3.1** | **131.9, 1.76** | **1.80 M** |

**With the client splicing, the window down to 7 costs no rate.**
Round 94 lost 3.5% at 7 (68.29 against 70.79); here 75.16 against
75.12.  What it buys is what it bought then: 7 MiB of the median, the
peak from 11,040 pages to 4,364, and 9 GB/s of memory (1.88 to 1.76
bytes a byte).  The WRITE's stage falls from 30.0 to 21.2 MiB and the
assembly's from 10.7 to 8.5, while the sockets' queues double, as in
round 94: a paused connection's bytes wait in its socket.

Why the price is gone is not measured here.  Round 94 laid it on the
drives running dry under the client's bursts at the window, about a
record's arrival away from their next WRITE; the per-connection rate
rose only from about 3.0 to 3.1 GB/s, so a record's arrival is not
much faster.  The likelier change is that the drives are now nearer
their capacity and their queue longer, so a window of 7 still leaves
them about two WRITEs: 21.2 MiB at 75 GB/s is 0.28 ms a WRITE at the
drive, against round 94's 0.24 at 7.

**Windows of 6, 5 and 4 follow** (`t2-r545` to `r547`), to find where
the rate starts to fall.
