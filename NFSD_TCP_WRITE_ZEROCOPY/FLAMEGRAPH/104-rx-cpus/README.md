# Round 104: what the receive rings' CPUs do on reads

Round 103 settled `xps_rxqs` on tardis2, and two-port reads on all
eight drives went from 54 to 60.5 GB/s. On those reads all eight
receive-ring CPUs (0, 2, 4, 6 on port a; 56, 58, 60, 62 on port b)
were 81 to 93% busy, all of it softirq, while the transmit rings'
CPUs were 11 to 22% and the drives 85 to 87%. The frames a read
server receives are the client's READ calls and ACKs, a small
fraction of the bytes, so the question is what that softirq time is.

## The runs

`run645.sh`, on build 531 at the settled setting (xps_rxqs, xeu
Release 17, the DRC `nfsd.ko` in `updates/`): t2-r645 reads, then
t2-r646 writes for the same profile with data coming in, eight
drives on two ports, `WARM=10 RUNTIME=40`. In each run's window it
records the sixteen ring CPUs with call graphs for 8 s at 1999 Hz,
and reads both ports' per-ring counters and `/proc/softirqs` on
either side of it, so packets and softirqs a second can be set
beside the profile.

## t2-r645 and r646: profiled before the load, not in it

[run645.out](run645.out): t2-r645 read 60.51 GB/s (30.21 / 30.30),
ring drops 249, the receive CPUs 79 to 94% busy; t2-r646 wrote 78.09
GB/s (39.05 / 39.04), the receive CPUs 29 to 54%. Both at the settled
xps_rxqs; no drive stepped down, no new kernel warning.

**The profiles are not of the load.** The runner started `perf` 75 s
after twoport.sh, as round 103's queue trace did, and the load began
later: in t2-r645 the profile ran from 1794684305.7 to 318.4 and the
load's first second was 342.8. It caught twoport.sh's flow placement:
a short write job, with the placement's `bpftrace` on
`net:napi_gro_receive_entry` running on the receive CPUs (its program,
`trace_call_bpf`, the nofault copies and their faults are 10% of them).
Round 103's queue traces had the same fault (its README is corrected).
The runner now waits for the load's 25th second (`wire-sec.tsv`'s
rows) and traces each connection's transmit queue after the profile.
