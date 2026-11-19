# Round 158: IOPS on the stack with the rtc move fix

Mike, 19 November: "yes, run round 158".  tardis2 on round 157's stack
(sunrpc `7ACE0A0B5F50F2537453F12`, with `cae2f2ea2e5b1`, "SUNRPC: Run a
moved rtc connection's work item on its new CPU"), the IOPS setting
with no spill.  Four runs with no tracing, `../155-rtc-placement/
placement.sh 1222 1223 1224 1225` (round 151's counters, the rtc
placement every second: debugfs reads only), then one with `CONN=1`
(t2-r1226, 2 s of `conn.bt`) to see each moved connection's work item
run on its new CPU.  Round 156's runs on the stack before: 3.04 to
3.49 M, worse with more rebalancer moves.  `placement.sh` runs
`conn.bt` only with `CONN=1` from this round: its bpftrace slows the
run it traces.
