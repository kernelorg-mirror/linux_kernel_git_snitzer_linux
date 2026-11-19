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
