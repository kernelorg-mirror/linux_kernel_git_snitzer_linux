# Round 150: the server's CPU a READ, by stage and by cache line

Mike, 19 November: "should we just focus on reducing CPU time please?"
then "yes, cancel round 148 and run 149, then the server breakdown
using c2c on the server".  The server spends about 15.7 us of CPU a 4
KiB READ (64 CPUs, 85 to 89% busy at about 3.6 M IOPS), where local fio
on the same drives costs about 3.4 us (18.78 M IOPS, cores-bound, round
108).  It has never had a perf c2c run; round 145's two shared lines on
the client were worth 0.55 us a READ.

Round 148 is cancelled: its script was stopped during its first run
(t2-r1185, nconnect 12, left to finish), so it has no result.

**perf c2c on tardis2 needs ARM SPE**: `perf c2c record` says "no PMU
supports the memory events"; `arm_spe_pmu.ko` is built and not loaded.
`spe-probe.sh`, through `step.sh` after t2-r1185 and before round 149:
`modprobe arm_spe_pmu`, and whether a PMU appears (the firmware must
describe SPE's interrupt in the MADT).  If none, the breakdown is by
stage from a call-graph profile only.
