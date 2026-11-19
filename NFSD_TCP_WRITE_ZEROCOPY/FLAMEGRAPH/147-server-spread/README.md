# Round 147: what fills the server's CPUs, and what leaves some idle

Mike, 19 November: "how might we spread load more efficiently on the
server? is updating the spill over algorithm a way to more efficiently
balance cpu load?", then "yes, queue the server perf measurement after
round 146".  In every IOPS run, 14 to 21 of the 48 work CPUs (8-55) sit
under 80% busy while up to 30 are at 97% or more (t2-r1174: 52 to
100%); the 16 RX and spill CPUs (0-7, 56-63) are 87% busy, 43% of it
softirq.  So the spill, which only decides when the RX CPUs take rtc
work items, is not the lever.  From the code: the unit placed is a
connection (24, one rtc work item each); `svc_rtc_pick_least()` counts
work items placed, not load; the rebalance moves one connection every
250 ms.

`spread.sh 1182`: one 4 KiB random READ run at iodepth 96 (the client
on its `updates/` stack) and from its eighth second 8 s of `perf record
-a` at 999 Hz on the server without call graphs
(`/dev/shm/zc-s147-1182.data`), read by CPU into rtc work items
(kworkers), nfsd threads, softirq, other and idle, beside each CPU's
busy; each CPU's symbols of 2% or more.  It decides between placing by
measured load and letting a hot connection's work run on more than one
CPU.

## The run (t2-r1182)

3.19 M IOPS (port a 1.22 M: its drives read 0.6 to 2.3 GB/s each, one
slow; port b 1.96 M), the server 76% busy (busiest 100%).  Two mistakes
in `spread.sh`'s first reading, corrected from the kept data: `perf
script -F cpu,comm,sym` prints no symbol without `ip`, so `spread.txt`
went by command name only; and perf samples on cycles, which stop in
WFI, so its "idle" is no measure of idle (mpstat's busy is).

Nearly every work CPU's samples are in rtc kworkers (nfsd threads 0%,
`rtc_read` Y).  By mpstat, 15 of the 48 work CPUs were 95% busy or more
(99% on average) and 23 were 70% or less (61% on average).  By what they
run, from `perf script -F comm,cpu,ip,sym` grouped by symbol family:

| Work CPUs | Busy | lock | net rx | net tx | fs, block, NVMe | nfsd, sunrpc | other | sched + idle |
|---|---|---|---|---|---|---|---|---|
| 15 hot | 99% | 7.6% | 12.3% | 4.7% | 7.5% | 23.8% | 43.8% | 0.4% |
| 23 cool | 61% | 7.4% | 10.6% | 4.2% | 6.7% | 22.3% | 41.2% | 7.6% |

**The hot and the cool CPUs run the same work in the same mix**; the
cool ones run less of it and sleep between (sched and idle 7.6%).  No
kind of work (softirq, a lock, the drives) piles onto some CPUs: the
difference is how much traffic the connections placed on each CPU
carry.  So the lever is where connections go, by their load, or more
and smaller connections, not the spill CPUs and not splitting a
connection's work by kind.
