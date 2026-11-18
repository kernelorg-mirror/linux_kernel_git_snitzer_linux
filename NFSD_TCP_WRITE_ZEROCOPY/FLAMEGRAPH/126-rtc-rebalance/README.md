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

**Restarted on round 125's setting (Mike: "I want 126 to build ontop of
the 125 I just settled").**  `settled-iops.sh` holds
`tcp_limit_output_bytes` 4 MiB since `e4fcb2f38764a`; t2-r1034 was
already running on the old file at 256 KiB and does not count.  The
A/B runs again as `ab-rebal.sh 1038:250 1039:0 1040:250 1041:0`.

## Result (18 November, 08:58 to 09:12)

| Run | rebalance_ms | M IOPS (ports) | p99 a; b (ms) | Busy | us a READ | Work CPUs min / median / max | Ring CPUs min / median / max | Moves |
|---|---|---|---|---|---|---|---|---|
| t2-r1038 | 250 | 2.91 (1.45 / 1.47) | 3.10; 3.03 | 78% | 17.15 | 58 / 81 / 91 | 64 / 78 / 94 | 24 |
| t2-r1039 | 0 | 2.86 (1.34 / 1.52) | 3.00; 2.87 | 76% | 17.01 | 55 / 85 / 95 | 26 / 75 / 100 | 0 |
| t2-r1040 | 250 | 2.91 (1.52 / 1.39) | 2.83; 3.29 | 77% | 16.93 | 59 / 82 / 92 | 59 / 77 / 100 | 21 |
| t2-r1041 | 0 | 2.88 (1.48 / 1.40) | 2.90; 3.19 | 77% | 17.11 | 46 / 85 / 94 | 24 / 77 / 100 | 0 |

On: 2.91 M and 17.04 us a READ; off: 2.87 M and 17.06 (with round
125's two 4 MiB runs, 2.885 M).  +1.4% in pairs, both pairs the same
way, about the spread; CPU a READ level.  The least busy work CPU 58 to
59% on against 46 to 55% off, the least busy ring CPU 59 to 64% against
24 to 26%; one CPU still reaches 100% on.  No new kernel warning.

## Held, and tcp_limit_output_bytes again under it

Mike, 18 November: "set svc_rtc_rebalance_ms to 250 in settled-iops.sh
and then please do 3 quick additional runs using rebalancer with
tcp_limit_output_bytes back to 256K and then 512K and then 1 MiB".
`settled.sh` holds `svc_rtc_rebalance_ms` (0 at rest,
`EXP_RTC_REBALANCE_MS` for a run) and `settled-iops.sh` 250.  Runs:
t2-r1042 `EXP_TCP_LOB=262144`, r1043 524288, r1044 1048576, against
r1038 and r1040 at 4 MiB.
