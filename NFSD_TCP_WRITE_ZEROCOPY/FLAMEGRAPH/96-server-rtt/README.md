# Round 96: the server's share of the client's round trip

**The ask (Mike, 12 November): "go ahead, measure the server's share of
the RTT."**  Round 95 found the client's 24 connections `rwnd_limited`
99.5% of the time, the server advertising 244 KiB a connection, and
the loaded round trip 85 to 93 us against a 13 us floor: the rate is
the window over the round trip, and the round trip is mostly not the
wire.  This round measures what the server adds to it, on the settled
two-port loan setting (the budget 64, the window of WRITEs per export
0), before any code.

## What is measured ([rttsplit.bt](rttsplit.bt), [rttsplit-read.py](rttsplit-read.py), [run504.sh](run504.sh))

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
