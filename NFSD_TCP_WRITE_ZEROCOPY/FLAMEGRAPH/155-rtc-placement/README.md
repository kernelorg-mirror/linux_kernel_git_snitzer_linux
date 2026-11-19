# Round 155: where the rtc balance puts the connections

Mike, 19 November: "wasn't spill introduced purely for IOPS?  shouldn't
it be disabled for all now?  but yes: do add that snapshot and run it
with no spill as round 155".

**The spill is gone from the IOPS setting.**  `settled.sh` never held
it (none, weight 2), so the WRITE and READ passes ran without it; only
`settled-iops.sh` added it (round 116), and round 154 found it costs
1.05 us of the server's CPU a READ at the same IOPS.  `settled-iops.sh`
now leaves both at `settled.sh`'s.

**The question.**  Round 154's t2-r1214 (no spill) ran 3.29 M against
t2-r1212's 3.64 M, steadily from its second second, with both hosts
less busy and ten of nfsd's CPUs (8-55) at 69 to 83% while others were
full.  Each connection's work runs on the CPU the rtc balance places it
on (RFS takes its frames there), 96 connections on 48 CPUs, rebalanced
every 250 ms: an uneven placement would leave some CPUs full and their
connections' jobs slow, others idle.

`placement.sh 1215 1216`: two IOPS runs (round 151's `packets.sh`),
and through each, every second, nfsd's `rtc_stats` (debugfs: `placed
cpu:n`, each CPU's connections, and `rebalanced`).  `placement.awk`
gives each second's spread over nfsd's CPUs and, over the mpstat
window, CPUs grouped by their mean connections beside their busy.

**The spill's code is gone at the tip (Mike, 19 November: "please revert
all code changes associated with the spill at the tip of the tree"):**
`143e3df791c88`, "SUNRPC: Drop the rtc balance's spill onto other CPUs",
reverts `219a15abdd219` and the rebalancer's use of the spill CPUs from
`4c084f9754fe1`; `svc_xprt.o` builds at W=1 with no warning.  tardis2's
sunrpc still has the two parameters, at none and weight 2, which is the
same as not having them; this round runs on it.
