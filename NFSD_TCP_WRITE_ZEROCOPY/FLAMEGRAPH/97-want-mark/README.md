# Round 97: the want mark, by the round trip

**The ask (Mike, 12 November): "run the mark A/B".**  Round 96 found
85% of the server's ACKs sent by nfsd's read, 34 us after the segment
arrived, and the read held by `svc_tcp_rx_want_eighths` (6, round 25,
settled): while a loaned record is incomplete no thread is woken until
six eighths of the open window has arrived.  The client's rate a
connection is its 244 KiB window over the round trip, and the mark is
in the round trip.  Round 25 chose 6 over 4 on one port at 40 GB/s
with the drives the limit, for 5 us of nfsd's CPU a WRITE and one ACK
fewer; the round trip cost nothing there.

[run510.sh](run510.sh): the marks 6, 4, 2 in turns, twice each
(`t2-r510` to `r515`), each a settled two-port loan run with
`WANT_EIGHTHS=<mark>` (twoport.sh sets it for the run and puts 6
back), round 96's `rttsplit.bt` for 12 s of the window.
[markread.py](markread.py) reads each run's rate, nfsd's and the
rings' cycles a WRITE (twoport's perf stat of the window, idle cycles
as the core counts them), wakes and switches a WRITE, the held median
and peak, and the client's round trip from ss.

## Result: the server's delay halves, the round trip does not

[run510.out](run510.out).  Every run clean: 12/12 flows, no drop, no
drive stepped down, no new kernel warning, 26 to 93 retransmits.  Two
runs a mark, the first and second of each:

| mark | GB/s | ack delay, the read's ACK | first unread byte's wait | at the wall | reads a WRITE | nfsd k cycles a WRITE | rings | held median | peak | client RTT (ss) |
|---|---|---|---|---|---|---|---|---|---|---|
| 6 (settled) | 70.18, 70.32 | 32.0 us | 34.6 us | 10.8% | 4.9 | 373, 376 | 245 | 55 MiB | 7,079, 7,134 | 78, 78 us |
| 4 | 70.86, 71.39 (+1.2%) | 25.7 | 27.4 | 5.3% | 6.0 | 402, 401 (+7.5%) | 249 | 55, 56 | 7,416, 7,149 | 85, 82 |
| 2 | 72.13, 72.21 (+2.7%) | 14.4 | 11.9 | 2.6% | 7.6 | 444, 447 (+19%) | 253, 258 | 57 | 7,244, 7,397 | 71, 80 |

- **The mark does what round 96 said:** at 2 the read comes 23 us
  sooner, the read's ACK 18 us sooner (32 to 14 us), and a quarter as
  many segments find the window's wall.  The cost is round 25's: 2.7
  more reads a WRITE, 70k more cycles of nfsd a WRITE (19%; nfsd's
  CPUs go from about 15% to 18% busy), 2 to 3 MiB more held at the
  median.
- **But the client's round trip does not fall with it** (78 us at 6,
  71 to 80 at 2, from ss at the window's end), **and the rate rises
  2.7%, not the 25% the window over the round trip would give** if the
  18 us had come off the round trip.  The bytes the server stopped
  holding unacknowledged wait somewhere else now: the window stays
  full of them, the round trip stays at the window over the rate, and
  the rate is set by something the server's ACK was not holding back.
  By Little's law a full window of 244 KiB a connection at 72 GB/s is
  81 us whatever the server does, so the round trip of round 95 was
  the window's consequence, not its cause, except for the 2.7%.
- What the ceiling is, these runs do not say: round 56 found about 73
  GB/s flat from 512 to 640 KiB of held buffer at rings of 256, round
  94 found the drives at 1.5 to 3 writes outstanding each where two
  fill one (8 x 10.2 GB/s is 82), and the client's send side has not
  been measured.  The rate at mark 2, 72.2, is round 56's ceiling.

**What this leaves for the footprint.**  The window cannot shrink
without the rate falling at mark 6: round 56 had 61.9 GB/s at 384 KiB
and 70 at 512.  With the server's 18 us given back, the same 384 KiB
window might carry more: 178 KiB over a shorter round trip.  That is
the one test left on this line, and it is the one that would move the
footprint (round 56's 384 KiB peaked at 9,881 pages against 12,648):
**the held buffer at 384 KiB with the mark at 2 and at 6, two runs
each.**  If 384 KiB at mark 2 holds 70 GB/s, the footprint comes down
with the rate kept; if it does not, the window is not the lever and
the next round goes to what the ceiling is.

## The second step: the held buffer at 384 KiB

[run516.sh](run516.sh): `RCVBUF=393216` with the mark at 2 and at 6
in turns, twice each (`t2-r516` to `r519`), otherwise the mark A/B's
runs.  The question is whether the 18 us the mark gives back lets a
384 KiB held buffer (a 178 KiB window) carry what 512 KiB does, so
that the footprint comes down at the rate kept.

### Result: the small window gets its rate back, the footprint stays

[run516.out](run516.out) (`t2-r516` to `r519`), every run clean (12/12
flows, no drop, no drive stepped down, 21 to 38 retransmits, no new
kernel warning):

| held buffer | mark | GB/s | read's ACK after arrival | at the wall | nfsd k cycles a WRITE | held median | peak | MiB held a GB/s | client RTT |
|---|---|---|---|---|---|---|---|---|---|
| 512 KiB (settled) | 6 | 70.18, 70.32 | 32 us | 10.8% | 373, 376 | 55 MiB | 7,079, 7,134 | 0.78 | 78 us |
| 512 KiB | 2 | 72.13, 72.21 | 14 | 2.6% | 444, 447 | 57 | 7,244, 7,397 | 0.79 | 71, 80 |
| 384 KiB | 6 | 55.68, 60.11 | 39, 33 | 25.7% | 371, 372 | 43, 45 | 6,737, 6,993 | 0.76 | 58, 60 |
| **384 KiB** | **2** | **69.04, 69.11** | 11.5 | 4.2% | 459, 459 (+23%) | **52, 53** | 7,213, 7,213 | 0.76 | 56, 53 |

- **The mechanism is confirmed where the window binds.**  At 384 KiB
  the mark of 6 gives 56 to 60 GB/s (round 56's 61.9) with a quarter
  of the segments at the wall; the mark of 2 gives 69, within 1.5% of
  the settled 512 KiB, with the read's ACK 11.5 us after arrival.  A
  small window needs the server's ACK soon; the settled window does
  not.
- **But the footprint follows the rate, not the window.**  Every cell
  holds 0.76 to 0.79 MiB at the median for each GB/s it carries,
  whatever the window and the mark: 55 MiB at 70, 52 at 69, 44 at 58.
  The TCP window is where the client's bytes queue, not where the
  server's pages wait: a loaned page's life (round 94) is 43% at the
  drive, 16% posted in the rings, 21% in a record's assembly, and
  about 17% on the way back, and the window changes only the socket's
  queue and part of the assembly.  Held pages are the rate times the
  time a page lives, about 0.75 ms, and the window moved that by 4%.
- **So the receive window is not the lever for the 32 MiB**; at 72
  GB/s the L3 needs a page to live 0.45 ms instead of 0.75.  Where the
  0.75 ms is, and what of it can move, is the stage split below.

### The stage split at 384 KiB (t2-r520, r521)

[run520.sh](run520.sh), [run520.out](run520.out): round 94's
`heldsplit.bt` beside a run at 384 KiB with the mark at 2 and one at
the settled 512 KiB with the mark at 6, no socket sampler (its pages
are in the rest row).  Pages at the median:

| stage of a loaned page's life | 512 KiB, mark 6 (`r521`, 70.76 GB/s) | 384 KiB, mark 2 (`r520`, 68.75 GB/s) |
|---|---|---|
| posted in the rings | 512 (8.0 MiB) | 512 (8.0 MiB) |
| a record assembling | 683 (10.7) | 717 (11.2) |
| in the WRITE at the drive | 1,364 (21.3) | 1,273 (19.9) |
| the way back (and the sockets) | 547 (8.5) | 503 (7.9) |
| to WRITE, to release | 107 (1.7) | 106 (1.7) |
| **held** | **3,435 (54 MiB)** | **3,221 (50 MiB)** |

The smaller window and the earlier read moved no stage but the
drive's, and that one by the rate (-3% at -2.8%).  The assembly did
not shrink: a record's pages wait for the rest of the record, which
the client sends at its pace, not the window's.  **The receive
window is where the client's bytes queue, not where the server's
pages wait.**  The mark stays 6 and the held buffer 512 KiB
(`settled.sh`), nothing changed on the host.

## What this round closes, and what is left

Rounds 94 to 97 tried every bound on the receive side (the budget,
a window of WRITEs a drive, the TCP window, the read's mark) and
each trades rate for pages along one line, about 0.77 MiB held for
each GB/s carried, because held pages are the rate times the 0.75 ms
a page lives and none of them shortens that life.  At 72 GB/s the L3
needs 0.45 ms.  Of the 0.75: the drive's queue is 0.3 ms and needs
two writes queued a drive to run flat out (16 MiB of the 21); the
rings are 8 MiB posted whatever the rate; the assembly, 11 MiB, is
the client's pace; the way back, about 8 MiB, is the release path
and the pool's refill, the one stage that is wholly the server's and
has been worked for cycles but never for time.  The two stages left
that are not the drives' or the client's are the way back and the
rings' size; together they are about 16 MiB, and 32 at 72 GB/s is
not reachable by tuning this design.
