# Round 159: both rtc placement fixes, the passes and IOPS

Mike, 19 November: "stop 158, just redo as 159".  Round 158 stopped
after t2-r1222 (3.27 M, port a 1.47 M) and its run in flight, t2-r1223:
the move fix alone left about six connections never placed.  A
connection is placed only on a receive kick in softirq, and a busy one's
segments arrive while its work item holds the socket and are processed
at its release, in process context: never placed, it runs where its
work item last ran, out of the balance's sight (t2-r1218: five, on CPUs
10 and 30).  `efe70d0679e89`, "SUNRPC: Place an rtc connection on a
receive outside softirq too" (`Fixes: 6db0c38527abe`), places it there.

`run159.sh`: the stack from the tip with both fixes (`cae2f2ea2e5b1`
and `efe70d0679e89`) built and swapped in, then the WRITE pass
(t2-r1224) and the READ pass (t2-r1225), four IOPS runs with no tracing
(t2-r1226 to r1229) and one with `conn.bt` (t2-r1230).
