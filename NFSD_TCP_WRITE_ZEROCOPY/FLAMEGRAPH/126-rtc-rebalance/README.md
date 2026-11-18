# Round 126: the rtc rebalancer

Mike, 18 November: "might we look at ensuring more even placement than
the spill mechanism can provide?", then "implement the rebalancer after
r1033, quick A/B".

Why: a work item is a whole connection, about 45% of a CPU at 2.9 M; 96
connections at spill weight 1 put about 15 on ring CPUs and 81 on the 48
work CPUs, so some hold one and some two.  The work CPUs ranged from 51
to 95% busy in t2-r1029 to r1031, the ring CPUs from 13 to 100%.

`4c084f9754fe1` "SUNRPC: Move rtc connections off the busiest CPU as
they run": `svc_rtc_rebalance_ms` (0 off): each pass compares the CPUs
the work items may run on by busy (kcpustat) since the last, and moves
one connection from the busiest that holds any to the least busy when
the gap is past a connection's share plus `svc_rtc_rebalance_gap` (10)
points.  rtc_stats counts the moves.  Built into
`/var/tmp/svcstack-541-rebal` (sunrpc `32BA58F914BEB5F3CF491F8`), W=1
clean.

The quick A/B: `ab-rebal.sh 1034:250 1035:0 1036:250 1037:0` under
`settled-iops.sh`.
