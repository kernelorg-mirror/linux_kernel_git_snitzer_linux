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
them about two WRITEs: 21.2 MiB at 75 GB/s is 0.30 ms a WRITE at the
drive, against round 94's 0.24 at 7.

## Below 7 (t2-r545 to r547)

[run545.out](run545.out), the same script, `SWEEP="6 5 4"`.  Every run
clean as above.  The whole curve, with round 94's rate at each window
beside it (`t2-r494` to `r500`, the client copying):

| window | GB/s | round 94 | held median (MiB, L3) | peak | in the WRITE | assembly | sockets | memory GB/s, bytes a byte | client retransmits |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 75.12 | 70.79 | 62, 1.94 | 11,040 | 30.0 | 10.7 | 1.6 | 141.0, 1.88 | 58 |
| 10 | 75.14 | | 60, 1.89 | 5,097 | 24.5 | 9.7 | 1.9 | 138.5, 1.84 | 236 |
| 8 | 75.10 | | 58, 1.80 | 4,670 | 22.3 | 9.0 | 2.5 | 136.1, 1.82 | 310 |
| 7 | 75.16 (0) | 68.29 (−3.5%) | 55, 1.73 | 4,364 | 21.2 | 8.5 | 3.1 | 131.9, 1.76 | 312 |
| 6 | 73.86 (−1.7%) | 66.14 (−6.6%) | 52, 1.61 | 3,964 | 18.7 | 8.2 | 3.5 | 117.7, 1.60 | 276 |
| 5 | 72.77 (−3.1%) | 64.79 (−8.5%) | 47, 1.47 | 3,541 | 15.5 | 7.4 | 4.0 | 103.2, 1.43 | 283 |
| 4 | 69.94 (−6.9%) | 60.28 (−15%) | 42, 1.32 | 3,144 | 11.7 | 6.6 | 4.7 | 84.3, 1.22 | 17 |

- **The knee is at 7.**  Down to 7 the rate does not move; each step
  below costs 1 to 4%, about half what round 94's did at the same
  window.  So the faster client does change the calculus: the window's
  price was in good part the client's, and what is left below 7 is the
  drives running under two WRITEs (18.7 MiB at 73.9 GB/s is 0.27 ms a
  WRITE at the drive at 6).
- **Memory falls faster than the rate below 7:** 6 gives 1.60 bytes a
  byte, round 98's target for 81, for 1.7% of the rate; but the rate
  does not rise with the memory freed, so at these windows the drives'
  feed, not memory, holds it.
- **The client's retransmits rise at 5 to 10** (236 to 312 against 58
  off), as in round 94 (138 and 215 at 6 and 7) with nothing dropped
  anywhere; at 4, 17.  Of about 9 M segments a run it is small, and the
  cause (TCP's probes on paused connections is the likeliest) is not
  shown.

## Window 7 again (t2-r548 to r550)

[run548.sh](run548.sh) is `run494.sh` with the budget as `WRITES`;
[run548.out](run548.out), [run550.out](run550.out).  Every run clean as
above.

| | budget | GB/s | held median (MiB, L3) | peak | in the WRITE | assembly | sockets | memory GB/s, bytes a byte | denied | client retransmits |
|---|---|---|---|---|---|---|---|---|---|---|
| t2-r544 | 128 | 75.16 | 55, 1.73 | 4,364 | 21.2 | 8.5 | 3.1 | 131.9, 1.76 | 0 | 312 |
| t2-r548 | 64 | 74.97 | 56, 1.73 | 4,275 | 21.5 | 8.7 | 3.0 | 131.7, 1.76 | 0 | 345 |
| t2-r549 | 64 | 75.15 | 56, 1.74 | 4,307 | 21.5 | 8.6 | 3.0 | 132.2, 1.76 | 0 | 304 |
| t2-r550 | 128 | 75.10 | 55, 1.73 | 4,313 | 21.5 | 8.6 | 3.2 | 131.4, 1.75 | 0 | 313 |
| off, budget 64 (t2-r534 to r536) | 64 | 74.99 to 75.26 | 62, 1.93 | 7,145 to 7,359 | 27.5 to 28.3 | 10.1 to 10.2 | 1.6 | 140.4 to 141.6, 1.86 to 1.89 | 4.4 to 5.3% | 39 to 70 |

**Window 7 holds over four runs, at either budget:** 74.97 to 75.16
GB/s against 74.99 to 75.26 off, with 6 to 7 MiB less held at the
median, the peak from about 7,250 pages at the settled budget (11,040
at 128) to about 4,300, and 9 GB/s less through memory.  At the
settled budget it also ends the denials: seven WRITEs a drive is 56 of
the budget's 64, so the budget no longer binds and no transport waits
for credit.  The one cost seen is the client's retransmits, about 300
a run against 40 to 70, with nothing dropped anywhere.

## Where this leaves the next steps

- **Window 7 is the proposal for the settled setting** (`settled.sh`,
  `svc_tcp_rx_target_window` 7; Mike's to decide).  It does not raise
  the rate: memory is freed (132 GB/s of 148) but 75 holds, so at
  window 7 the drives' feed is what sets it.
- **Round 95's budget:** with the window at 7 the budget does not
  bind (no denial at 64 or 128); what is left of it is the bound on a
  record's pages for transports the window does not key (3,300 to
  3,500 unkeyed a run), and the reserve.
- **Round 97's mark:** the A/B of 2 against 6 is to be run on whichever
  window is settled; at 7 the rate is not the round trip's, so mark 2
  is expected to gain nothing and to cost nfsd's cycles.
- **The retransmits:** whether they are TCP's probes on paused
  connections is to be shown before the window is settled, since they
  are its one visible cost (below).

## What the retransmits are (t2-r551, r552)

[retx.sh](retx.sh): one settled two-port run at window 0 and one at 7,
budget 64, and over twoport's 40 s window every TCP counter of both
hosts (`/proc/net/netstat`, `/proc/net/snmp`) and the client's `ss
-tin` of its connections every second; [retx-read.py](retx-read.py),
[retx551.out](retx551.out).  Both clean: 75.14 and 74.88 GB/s, 0 ring
drops, no drive stepped down, no new kernel warning.  (tardis2's clock
was put right by NTP between `t2-r550` and these runs: their times
read 13:41 and 13:46, about six and a quarter hours after the earlier
ones.)

| over the window | window 0 (t2-r551) | window 7 (t2-r552) |
|---|---|---|
| client `RetransSegs` | 56 | 330 |
| client tail loss probes (`TCPLossProbes`) | 1,476 | 2,555 |
| client fast retransmits | 45 | 33 |
| client RTOs (`TCPTimeouts`) | 1 | 0 |
| **client DSACKs received / server DSACKs sent** | **2 / 2** | **289 / 289** |
| server `TCPWantZeroWindowAdv` | 26 | 375 |
| server windows advertised to zero, and from | 0, 0 | 1, 1 |
| connection-samples in backoff or with zero-window probes (ss) | 0 | 0 |

**They are spurious tail loss probes.**  At window 7 the client sends
about 1,080 more tail loss probes, and 289 of its 330 retransmitted
segments come back DSACKed by the server (`TCPDSACKOldSent` 289): data
the server already had.  Nothing was lost: no RTO, no backoff, no zero
window probing, and the server put a window to zero once.  The
mechanism is the one round 96 measured from the other side: 85% of
the server's ACKs are sent by nfsd's read, and a connection paused for
its export is not read, so its ACKs wait, the client's probe timeout
(about two round trips) fires, and it sends its last segment again.
The server's `TCPWantZeroWindowAdv` (375 against 26) is the same pause
seen from the receive buffer.

The cost is about 290 duplicate segments of 7.7 KB in 40 s, some 2 MB
against about 3 TB carried: nothing in rate or memory.  `ss` without
`-o` prints no timers, so the timers row the reader has is empty; the
counters above do not need it.

## Settled: window 7, budget 64 (Mike, 12 November)

`common.sh`'s `TWO_WINDOW=7` beside `TWO_WRITES=64`; `twoport.sh` sets
it for a two-port loan run (`TARGET_WINDOW=` gives another; not
`WINDOW=`, which GNU screen exports as its window's number, and which
held the first run of the A/B below, `t2-r553`, at 0 until it was
stopped) and puts 0 back; `settled.sh` holds 0 at rest and audits a
run's window.

## Round 97's mark again, at window 7 (t2-r554 to r557)

Round 97's [run510.sh](../97-want-mark/run510.sh) unchanged on the new
setting, marks 6, 2, 6, 2, with round 96's `rttsplit.bt` beside each
(which costs about 1% of the rate); [run554.out](run554.out).  All at
window 7, budget 64, no loan denied, no drive stepped down, no new
kernel warning; `t2-r554` placed 10 of its 12 flows and its rings
dropped 568 frames, the others 12/12 and 0.

| mark | GB/s | read's ACK after arrival | first unread byte's wait | at the window's wall | nfsd k cycles a WRITE | rings | held median | peak | client RTT | retransmits |
|---|---|---|---|---|---|---|---|---|---|---|
| 6 (settled) | 74.30, 73.92 | 33.4, 33.3 us | 30.5, 30.6 us | 11.1% | 383, 384 | 244, 246 | 55 MiB | 4,288, 4,285 | 65, 66 us | 900, 246 |
| 2 | 74.14, 74.28 | 16.7, 16.8 | 11.0, 10.9 | 3.9% | 451, 457 (+18%) | 254, 254 | 55 | 4,376, 4,367 | 66, 68 | 299, 233 |

**Mark 2 does what round 97 found it does, and buys nothing:** the
read's ACK comes in half the time and a third of the segments meet the
window's wall, but the rate is the same (74.21 against 74.11, within
the runs' spread), the client's round trip the same (65 to 68 us; round
97 had 78 at mark 6), the held pages the same, and nfsd spends 18% more
cycles a WRITE.  At window 7 the round trip is not what sets the rate,
so the mark stays 6.

## What this round closes

With the client splicing, the rate at the settled setting (window 7,
budget 64, mark 6) is 75 GB/s untraced, with the server's memory at
132 of 148 GB/s and 1.76 bytes a payload byte; neither memory, the
round trip, the budget nor the mark holds it.  The drives do: each
writes 9.3 to 9.5 GB/s of its 10.2 alone, with two WRITEs queued at it
at window 7.


## The held buffer and the connections a port at window 7 (t2-r558..r563)

The question: what holds the drives at 9.3 to 9.5 GB/s of their 10.2.
They are at their cap whenever two writes are queued
(round 94), and `t2-r534`'s depth (6 to 7.6% empty, 2% one deep, 91%
two or more) is 9.4 by arithmetic, so the shortfall is the feed.  At
`t2-r557` the client's connections were held by the server's
advertised window 97% of their busy time (`snd_wnd` about 240 KB at a
75 us round trip: 3.2 GB/s a connection, 78 on 24), and the buffer
and connections were settled when memory or the client was the wall.
[run558.sh](run558.sh) ([run558.out](run558.out)): one settled run
for each held buffer and connections a port, window 7, budget 64,
`WARM=10 RUNTIME=40`, with round 98's `inflight.py` (12 s) and the
mesh beside each.  (A first try left out `WRITES=64`, so `twoport.sh`
copied: `t2-r558..r560.copy-mode-stopped`, [run558.copy-mode-stopped.out](run558.copy-mode-stopped.out),
not to be read as loans; the script now refuses a run that did not
loan at window 7.)

| run | rcvbuf, conns a port | GB/s | ring drops | client retransmits | flows | rwnd_limited, mean snd_wnd | a drive empty / 1 / 2 / 3 / 4+ | memory, a byte | held median, peak |
|---|---|---|---|---|---|---|---|---|---|
| `t2-r558` | 512 KiB, 12 (settled) | **75.27** | 0 | 297 | 12/12 | 99.4%, 199 KiB | 6.4 / 1.9 / 2.4 / 3.3 / 86.0% | 132.6, 1.76 | 3,557, 4,322 |
| `t2-r559` | 640 KiB, 12 | 75.30 | 2,763 | 3,196 | 12/11 | 92.3%, 236 KiB | 6.2 / 1.7 / 2.4 / 3.1 / 86.6% | 134.4, 1.78 | 3,661, 4,489 |
| `t2-r560` | 768 KiB, 12 | 73.23 | 9,219 | 9,971 | 12/12 | 78.3%, 285 KiB | 8.5 / 2.2 / 2.7 / 3.4 / 83.3% | 127.0, 1.73 | 3,777, 4,715 |
| `t2-r561` | 512 KiB, 16 | 74.99 | 1,251 | 1,716 | 16/16 | 96.6%, 178 KiB | 6.6 / 2.0 / 2.7 / 3.5 / 85.1% | 134.0, 1.79 | 3,757, 4,733 |
| `t2-r562` | 768 KiB, 16 | 70.46 | 12,503 | 13,193 | 14/16 | 66.7%, 261 KiB | 12.1 / 2.8 / 3.4 / 4.1 / 77.5% | 114.5, 1.63 | 3,992, 4,777 |
| `t2-r563` | 512 KiB, 12 (settled, again) | **75.13** | 0 | 280 | 12/12 | 97.3%, 182 KiB | 6.3 / 2.0 / 2.6 / 3.3 / 85.8% | 133.1, 1.77 | 3,556, 4,380 |

No loan denied, no drive stepped down, no new kernel warning in any.
Every run's `drives.txt` says "PAST THE FAST REGION" (610 to 617 GB a
drive): twoport's flag is at 600 GB (`DRIVE_GB`) against the 625 a
drive takes at full rate, and no drive's per-second rate fell, so it
does not decide these figures.

- **The window opens and the client is held by it less, and the rate
  does not rise.**  640 KiB gives 19% more window and 92% in place of
  99%, at the same 75.3.  16 connections give 20% more window in all,
  at 75.0.
- **Past that, the client's bursts overrun the rings.**  At 768 KiB
  the rings (128 entries) drop 9,219 frames on 12 connections and
  12,503 on 16, the retransmits leave the drives empty 8.5 and 12% of
  the time, and the rate falls to 73.2 and 70.5, with memory at 127
  and 115 GB/s, well inside its 148.  The window is also what keeps
  the client's bursts inside what the rings take; 512 KiB on rings of
  128 is the largest that does, which is how round 56 chose it.
- **So the server's window is not what holds 75.**  Each connection
  sends as fast as the window lets it, but more window in every form
  delivers no more: the rate the server takes the data in is set
  elsewhere.  The settled setting stays (512 KiB, 12 connections a
  port), its two runs 75.27 and 75.13.

## When a drive runs empty, its next WRITE is still arriving (t2-r564..r572)

[hol.bt](hol.bt), read by [hol-read.py](hol-read.py), run by
[run564.sh](run564.sh) beside `inflight.py` on settled runs (window 7,
budget 64, 512 KiB, 12 a port).  A gap opens when a drive's last request
completes (g) and closes at its next issue; the WRITE (W) that issue is
for, found by the nfsd thread that queued its bio, is classed by where
it stood at g: A in nfsd before its I/O, B read and not yet writing,
C being read off its connection (its first region skb read, not yet its
last), D not begun with its connection still reading the record before
it (D1 for another export, D2 the same: head-of-line), E not begun with
its connection idle, F no WRITE found.

The trace took five versions to stand; each earlier one is kept, with
its runs:

| version | runs | what was wrong |
|---|---|---|
| [hol-v1.bt](hol-v1.bt) | `t2-r564` | one depth count a drive, `+=` and `-=` from many CPUs: lost updates, a tenth of the empty time seen |
| [hol-v2.bt](hol-v2.bt) | `t2-r565` | perf tracepoints: a completion's interrupt during an issue's program is skipped (`bpf_prog_active`), its request in flight for ever; 28 gaps in 11 s |
| (v3, in [run566-hol-v3.out](run566-hol-v3.out)) | `t2-r566`, `r567` | a completion's interrupt inside an issue's `+=` on the same CPU (a local probe: 51,344 of each, the count left at 56); then two issues at once, one reading the other's deleted gap as 0, gaps from the trace's start (218 over 100 ms) |
| [hol-v4.bt](hol-v4.bt) | `t2-r568`..`r570` | one first read a connection: a read that takes one record's end and the next's start lost the second (F a quarter of the empty time, 22% of requests unowned) |
| [hol-v5.bt](hol-v5.bt) | `t2-r571` | a FIFO of first reads a connection fell behind (4 or more queued at nearly every match): every gap C; does not count |
| hol.bt | `t2-r572` | first and full read matched by xid |

What stands from v4 on: raw tracepoints, a request keyed by its pointer,
issues and completions counted apart per CPU, the gap taken before it is
read.  The trace's empty share a drive is 4.9 to 6.4% against
`inflight.py`'s 7.4 to 7.9% in the same runs.

| run | GB/s | C being read | D1 behind another export | D2 | E idle | A + B | F |
|---|---|---|---|---|---|---|---|
| `t2-r568` (v4) | 74.62 | 54.7% | 12.2% | 1.0% | 1.2% | 5.7% | 25.2% |
| `t2-r569` (v4) | 75.00 | 54.3% | 13.2% | 0.3% | 1.1% | 6.1% | 25.1% |
| `t2-r570` (v4) | 75.05 | 53.2% | 10.1% | 2.8% | 1.0% | 5.8% | 27.1% |
| `t2-r572` (xid) | 75.04 | **57.0%** | **14.2%** | 1.6% | 0.8% | 6.4% | 20.1% |

(shares of the drives' empty time).  F is still a fifth: of the records
read in full, 403,423 matched a first read by xid and 50,445 did not,
and 57k records a second reach `svc_xdr_recvfrom` in the trace against
the 71.5k 1 MiB WRITEs a second 75 GB/s is, so the trace still misses
records.  F's gaps have C's and A's shape (half under 64 us, 3 to 4%
at 256 us or more), not D1's (70% at 256 to 512 us), so F hides little
head-of-line.

- **A drive runs empty mostly while the WRITE that will feed it is on
  its way in.**  In 55 to 57% of the empty time the next WRITE's first
  bytes have been read and its last have not: it is read in full a
  median 32 us after the drive empties, and the gap is 32 to 256 us.
  A 1 MiB record over one connection at 3.2 GB/s (the window's rate)
  takes about 330 us; nothing of it reaches the drive until all of it
  is read.
- **Head-of-line is the smaller part: 10 to 14% (D1).**  Those gaps are
  long (256 to 512 us, a record's time on a connection), so they are few
  (2% of gaps) and weigh more than their count.
- **The server's own path is not it.**  A and B, the WRITE read and in
  nfsd, are 6% of the empty time in gaps of 8 to 16 us; E, a connection
  idle, 1%.
- So the drives wait on the arrival of a record at one connection's
  rate.  What would shorten that: a WRITE's record arriving faster (a
  wider window a connection, which the rings of 128 do not take past
  512 KiB: t2-r559..r562), records that are smaller (a smaller wsize:
  more of them, each sooner), or more WRITEs already read and waiting
  for each drive (the client keeping more in flight a drive).

## Twice the client's writers, now the client splices (t2-r573..r576)

[run573.sh](run573.sh) ([run573.out](run573.out)) is round 98's
`t2-r526` again: 32 writers a port (8 a drive) against the settled 16,
8 MiB blocks at iodepth 16, in turns, at the settled setting (window 7,
budget 64, 512 KiB, 12 connections a port), `WARM=10 RUNTIME=40`.
Round 98 found no gain with the client 70% busy and copying; the
client now splices and is 42% busy, and the trace above has the drives
waiting on WRITEs still arriving, so more WRITEs in flight a drive
might have one already read.

| run | writers a port | GB/s | a drive empty / 4+ | rwnd_limited | client CPUs | held median, most | to memory |
|---|---|---|---|---|---|---|---|
| `t2-r573` | 16 | 75.22 | 6.4% / 85.7% | 99.4% | 42% | 55 MiB, 3971 | 132.6 GB/s |
| `t2-r574` | 32 | 73.57 | 8.2% / 83.2% | 98.4% | 43% | 55 MiB, 4087 | 128.1 GB/s |
| `t2-r575` | 16 | 75.14 | 6.5% / 85.8% | 98.5% | 42% | 55 MiB, 4006 | 132.3 GB/s |
| `t2-r576` | 32 | 73.74 | 8.2% / 83.1% | 97.4% | 44% | 55 MiB, 4067 | 128.5 GB/s |

No loan denied, no ring drop, no drive stepped down in any.  **Twice
the writers lose 2% (73.66 against 75.18), and the drives are empty
8.2% of the time against 6.5%.**  The held pages are the same.  The
client's connections are held by the server's window in both, so the
extra WRITEs wait at the client: none of them arrives sooner, and a
drive takes eight interleaved streams instead of four.  The client's
writers are not the lever; 16 stays.

## A 512 KiB wsize (t2-r577..r582)

[run577.sh](run577.sh) ([run577.out](run577.out)): the client's
rsize and wsize (twoport.sh's `WSIZE`) at 1 MiB and 512 KiB, in turns,
at the settled setting otherwise (512 KiB held buffer, 12 connections a
port, 16 writers a port, 8 MiB blocks), the budget held at 8198 pages,
`WARM=10 RUNTIME=40`.  The window counts records an export, so 512 KiB
ran at window 14 (window 7's bytes) and at window 7 (half of them).
Every client mount took the wsize asked.

| run | wsize, window | GB/s | a drive empty / 4+ | client CPUs | held median, most | to memory |
|---|---|---|---|---|---|---|
| `t2-r577` | 1 MiB, 7 | 75.04 | 6.6% / 85.6% | 42% | 55 MiB, 3889 | 131.7 GB/s, 1.76 a byte |
| `t2-r580` | 1 MiB, 7 | 74.81 | 6.9% / 85.2% | 42% | 55 MiB, 3913 | 131.1, 1.75 |
| `t2-r578` | 512 KiB, 14 | 78.46 | 2.7% / 90.1% | 49% | 54 MiB, 3754 | 131.4, 1.68 |
| `t2-r581` | 512 KiB, 14 | 78.40 | 2.5% / 90.0% | 50% | 54 MiB, 3771 | 132.4, 1.69 |
| `t2-r579` | 512 KiB, 7 | 76.72 | 4.1% / 83.4% | 48% | 41 MiB, 2847 | 87.1, 1.13 |
| `t2-r582` | 512 KiB, 7 | 76.23 | 4.5% / 81.9% | 48% | 40 MiB, 2818 | 84.2, 1.10 |

No loan denied, no ring drop, no drive stepped down in any; twice the
loans admitted at 512 KiB, as there are twice the WRITEs.

- **512 KiB at window 14 carries 78.43 GB/s against 1 MiB's 74.93
  (+4.7%), at the same held pages.**  A drive's empty time falls from
  6.7% to 2.6%: the record that refills it is read in half the time,
  as the section above has it.  The cost is the client's CPUs, 50%
  busy against 42% for twice the RPCs, and twice the WRITEs through
  nfsd.
- **512 KiB at window 7 carries 76.48 (+2.1%) on a quarter fewer held
  pages (40 MiB, 1.25 the L3, against 55 MiB, 1.73) and a third less
  traffic to memory (1.10 bytes a byte written against 1.76).**  Half
  the bytes in flight an export: the drives are empty less often than
  at 1 MiB but are kept less deep (82 to 83% at 4 or more against
  85%), and what is held nearly fits the L3.
- Neither is a change to the server: the wsize is the client's mount.
  Window 7 at 1 MiB and 14 at 512 KiB are the same bytes, so a window
  counted in bytes rather than records would keep its meaning across
  wsizes.  Window 10 or 11 at 512 KiB, between the two, is not run.
