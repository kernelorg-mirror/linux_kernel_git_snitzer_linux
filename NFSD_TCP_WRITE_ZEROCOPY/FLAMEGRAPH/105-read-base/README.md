# Round 105: the READ base, profiled

**The method (Mike, 17 November: "its like you forgot the entire test
methodology used to optimize WRITEs.  We need that same approach applied
to READs"; "you cannot just keep testing 1 thing and expect it to unlock
all the gains"; "if balanced placement is ideal, then do it.. and find
out why it isn't performing with NFS").**  As for WRITEs: each round
profiles the READ strobe on the current base, ranks its costs a READ on
the work items' CPUs (8-55) and the ring CPUs, with the memory bytes a
byte sent beside them, changes the most costly thing, A/Bs it in turns,
and keeps it on a saving.  The base carries what should be right, and a
round finds why it does not perform yet, rather than dropping it.

**What the hardware leaves (round 104):** the drives read 104.58 GB/s
locally, the two ports send 98.31 (iperf3 `-Z`), memory carries about 140
GB/s with both directions mixed; NFS READs at the settled setting move
73 GB/s at 116.5 GB/s of memory.  iperf3 and NFS both carry about
3 GB/s a connection: iperf's 95 is 32 connections, NFS's 73 is 24.

**The base:**

- **One connection to a work CPU** (`svc_rtc_balance` Y over
  `svc_rtc_cpus` 8-55).  The hash shares CPUs (0.7 GB/s a shared
  connection at N, round 104); balanced placement removes that.
- **Each connection's frames on its work item's CPU** (RFS,
  `EXP_RFS=32768`): the ACKs are processed where the send runs, so the
  socket lock is not fought over (4.6% of the work items at N), and a
  READ's pages are freed on the CPU that allocated them, into its own
  per-CPU list (the zone lock is 7% of the work items at N).  RFS failed
  on 15 November (round 104, t2-r688) only through placement: a work CPU
  that took a steered receive was marked a receive CPU for good.
  `08b251d13aa0d` "SUNRPC: Do not mark a run-to-completion work CPU as a
  receive CPU" leaves the CPUs of `svc_rtc_cpus` unmarked; for build 534
  it is `sunrpc.ko` `54503B294CC030DD815A524` in
  `/var/tmp/zc-run-logs/sunrpc-rfsplace-534/` (the `sunrpc-nosplit-534`
  source with that change; every export the same).
- **nconnect 16 a port** (32 connections, as iperf's 95): nconnect 16
  lost at N (r740) when the ring CPUs saturated, with the receive on them.

The base is not the settled setting: [prof.sh](prof.sh) holds a run to
it (`EXP_RTC_BALANCE`, `EXP_RFS`, `NCONN`) and settles without it after.

About to run, through `step.sh`: the module swapped in with no client
connected, `settled.sh apply`, then `prof.sh 821`.
