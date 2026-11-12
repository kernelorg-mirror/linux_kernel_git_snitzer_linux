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
