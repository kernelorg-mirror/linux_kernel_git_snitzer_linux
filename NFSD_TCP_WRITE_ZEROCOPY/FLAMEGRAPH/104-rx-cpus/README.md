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
