# Round 96: the server's share of the client's round trip

**The ask (Mike, 12 November): "go ahead, measure the server's share of
the RTT."**  Round 95 found the client's 24 connections `rwnd_limited`
99.5% of the time, the server advertising 244 KiB a connection, and
the loaded round trip 85 to 93 us against a 13 us floor: the rate is
the window over the round trip, and the round trip is mostly not the
wire.  This round measures what the server adds to it, on the settled
two-port loan setting (the budget 64, the window of WRITEs per export
0), before any code.

## What is measured

[rttsplit.bt](rttsplit.bt), [rttsplit-read.py](rttsplit-read.py),
[run504.sh](run504.sh).

The card has no hardware timestamps (`ethtool -T`: software only), so
the clock starts when a segment reaches the stack (`tcp:tcp_probe`,
before it is queued).  The ring's own share, from the wire to the
poll, is bounded by the NAPI deferral (2 interrupts or 10 us) and the
poll cadence, which the trace records.  On nfsd's connections (local
port 2049), for 12 s of each run's window:

- **The ack delay:** from the arrival of a segment that finds
  everything before it acknowledged (`rcv_nxt == rcv_wup`) to the next
  transmit on the socket (`__tcp_transmit_skb`), by where that
  transmit is: *ring*, the softirq's ACK at arrival on a ring CPU;
  *read*, `tcp_cleanup_rbuf()` after nfsd's read, the ACK that opens
  the window; *nfsd other*, the reply or the delayed-ACK timer on
  nfsd's CPUs.  The client's round trip for that segment is this plus
  the wire both ways plus the ring's share.
- **The window:** the room left after each segment
  (`rcv_wup + rcv_wnd - end_seq`; under a frame is "the wall"), the
  window `__tcp_select_window()` chooses at each transmit by the same
  classes, and the time from a segment that closes the window to the
  first choice of 64 KiB or more.
- **nfsd's read:** the wait from the first unread byte's arrival
  (`rcv_nxt == copied_seq` at arrival) to `tcp_read_sock()`, the
  bytes waiting, the read's length and bytes.
- **nfsd's threads:** `sched_wakeup` to `sched_switch`.
- **The rings:** the gap between NAPI polls on the ring CPUs and the
  frames a poll.

Three runs, `t2-r504` to `r506`, `WARM=10 RUNTIME=40`, the trace on
CPU 63 from the window's start.

## Result: the ACK waits for the read, the read for the mark

[run507.out](run507.out) (`t2-r507` to `r509`, every probe fentry or
fexit).  The first three runs, `t2-r504` to `r506`
([run504.out](run504.out)), had a kprobe and a kretprobe on
`tcp_cleanup_rbuf()` and on `__tcp_select_window()`, which bpftrace
0.26 joins so that the kprobe's body runs at both ends and the
kretprobe's never (round 54 found this; checked again here: 78 entries
counted for 39 calls, no return): their split of the ACKs sent from
nfsd's CPUs and their window rows are wrong, the rest agrees with the
rerun.  `t2-r506` placed 11 flows on
port b's 12 entries and took 122 ring drops and 175 retransmits for it;
the other five runs placed 12/12 with no drop and 32 to 42 retransmits.
No new kernel warning (`t2-r506`'s log has `/boot`'s FAT volume
remounted, not ours).  69.3 to 70.0 GB/s with the trace on, against
71.2 to 72.2 untraced.

The three runs agree to within 10% on every row; `t2-r507`'s figures:

| | | mean | median bucket |
|---|---|---|---|
| the client's smoothed round trip (ss, 24 connections) | | 81 us median, 63 to 121 | floor 13 us |
| segments | 10.24 M in 12.96 s | | |
| at the wall (under a frame of room after it) | 1.14 M, 11.1% | | |
| **ack delay, the first unacked segment's arrival to the ACK** | | | |
| sent by the softirq at arrival, on a ring CPU | 252 k, 6.9% | **4.4 us** | 2-4 us |
| sent by `tcp_cleanup_rbuf()` after nfsd's read | 3.09 M, 84.7% | **34.4 us** | 16-32 us |
| sent from nfsd's CPUs otherwise (the reply) | 304 k, 8.4% | 34.5 us | 16-32 us |
| **the window chosen at those transmits** | | | |
| at arrival | | 170 KiB | 128-256 KiB |
| after the read | | 218 KiB | 128-256 KiB |
| with the reply | | 158 KiB | 128-256 KiB |
| **the window closed to a window of 64 KiB or more** | | | |
| by the softirq's ACK at arrival | 1.05 M of 1.14 M | 1.8 us | 1 us |
| by the read's ACK | 71 k | 15.6 us | 8-16 us |
| **nfsd's read** | 4.73 M reads | | |
| the first unread byte's wait for a read | | **37.6 us** | 16-32 us |
| bytes waiting at the read | | | 128-256 KiB |
| the read | | 6.6 us | 4-8 us |
| nfsd thread, wakeup to running | 3.19 M | 1.3 us | 1 us |
| NAPI poll gap on a ring CPU | 10.8 M polls | 16.7 us | 8-16 us; one frame a poll |

**Reading it.**

- **93% of the ACKs wait for nfsd's read, 34 us after the segment
  arrived.**  The softirq ACKs at arrival only 7% of the time: TCP's
  rule (`__tcp_ack_snd_check()`) sends at once only when the window to
  advertise is not smaller than the last, and with unread data in the
  queue it is smaller (170 KiB against 218), so the ACK is left to the
  reader, by design ("tcp_recvmsg() will send ACK otherwise").  The
  client's round trip is that ACK's delay plus the wire and its own
  stack: 34 of the 81 us smoothed, or 34 of the 68 above the floor.
- **The server's share is 40 to 60% of the round trip:** the 34 us to
  the read's ACK, plus the ring's share, from the wire to the poll,
  which has no timestamp but is bounded by the deferral (10 us) and
  the poll gap (17 us mean, one frame a poll).  The rest, 20 to 35 us,
  is the wire (13 us round) and the client.
- **The window is not what stalls the client; the unacknowledged
  bytes are.**  The server's window never falls far: 170 KiB even at
  arrival-ACKs, 218 after a read, of 244 at most.  When a segment does
  reach the wall, the softirq ACKs it within 2 us with 64 KiB or more
  (1.05 M of the 1.14 M wall hits).  The client is `rwnd_limited`
  because what it has sent and not yet had acknowledged fills its 244
  KiB: at 2.9 GB/s a connection, 34 us of unacknowledged bytes is 100
  KiB, and the rest of the window is on the wire and in the client.
  The rate a connection carries is the window over the ack delay plus
  the wire and client, 244 KiB / 81 us = 3.0 GB/s, the measured 2.9.
- **The read waits for the mark, not for a thread.**  The first unread
  byte waits 38 us for `tcp_read_sock()`; a wake takes 1.3 us and
  nfsd's CPUs are 15% busy.  `svc_tcp_rx_want_eighths` is 6 (round 25,
  settled): while a loaned record is incomplete, `svc_data_ready()`
  wakes no thread until six eighths of the open window has arrived, so
  that a record is taken up in fewer passes (5.6 against 6.6 at 4, 5 us
  of nfsd's CPU a WRITE).  Six eighths of 244 KiB at 2.9 GB/s is 63 us
  from the first byte; the reads of 128 to 256 KiB at the median are
  that mark.  Round 25 chose 6 on one port at 40 GB/s with the drives
  the limit, where the round trip cost nothing; on two ports at 70 GB/s
  the round trip is the limit, and the mark is in it.

So the chain is: the mark holds the read, the read holds the ACK, the
ACK holds the client.  It is one setting away from testing.

## The A/B suggested for round 97: the mark, by the round trip

`svc_tcp_rx_want_eighths` 6 (settled) against 4 and 2 (and 1, if 2
moves it), at the settled two-port setting, two runs each in turns,
`rttsplit.bt` on each: the rate, the client's round trip, the ack
delay, the held median and peak, and nfsd's cycles a WRITE (round 25's
cost is the other side).  If the round trip falls and the rate rises
at the same held buffer, the second step is the held buffer at 256 KiB
with the lower mark, for the footprint at the same rate (round 56's
256 KiB lost rate at the mark of 6 and the round trip of 80 us).  The
other way to the same ACK, a route metric `quickack 1` for the client's
subnets, ACKs every segment in the softirq at arrival with no change to
nfsd: no code, but twice the ACKs on the ring CPUs and the client; it
is the control if the mark does not move the round trip.  Neither
holds a page longer, so the footprint cannot grow from them.
