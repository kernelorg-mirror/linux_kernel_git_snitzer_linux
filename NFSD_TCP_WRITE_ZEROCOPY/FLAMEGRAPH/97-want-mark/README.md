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
