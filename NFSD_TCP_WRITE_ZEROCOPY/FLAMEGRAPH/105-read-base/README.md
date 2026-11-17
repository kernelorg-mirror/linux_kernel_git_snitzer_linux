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

## t2-r821: the base, profiled

**61.22 GB/s** (30.60 and 30.61), no new kernel warning, settled after.
The placement is as meant: 32 connections on 32 CPUs, one each.

- **RFS did what it was there for.**  In the work CPUs' profile the
  zone lock is gone (`rmqueue_bulk` 0.12% of their samples, against
  about 7% at N in t2-r779) and so is the socket's contention
  (`lock_sock_nested` 0.25%, against 4.57%).  The ACKs are processed on
  the work CPUs now (`tcp_v4_do_rcv` 21.9% of their samples,
  `tcp_clean_rtx_queue` 11.2%, `skb_release_data` 5.4% self), which are
  28% busy; the rings 33%.
- **But the server holds far too many bytes.**  Each socket's Send-Q is
  3.50 MB (max 4.19, the 4 MiB send buffer), 2.75 MB of it below TCP
  (`skmem t`: in the qdisc and the NIC), 390 segments unacked; TCP's RTT
  1.71 ms, against 0.27 at N.  32 sockets hold about 112 MB, 88 MB of
  it below TCP, three and a half times the 32 MiB cache: memory 136.2
  GB/s, **2.22 bytes a byte sent**, at the mixed ceiling of about 140.
  The client's sockets hold 0.12 MB unread (N 3.30): the backlog has
  moved from the client to the server.
- **What lets it:** `net.ipv4.tcp_limit_output_bytes` is 4 MiB in
  `settled.sh` (TCP small queues: the bytes a socket may have below
  TCP), so 32 sockets may put 128 MiB in the qdiscs.  The NIC's queues
  take 8 MB (BQL); the rest waits in fq_codel.

So the next fix on the base is the bound on bytes below TCP.
`settled.sh` gains `EXP_TCP_LOB`; [ab.sh](ab.sh) runs the base's strobe
at a given limit.  About to run, through `step.sh`: `ab.sh 822:524288
823:262144 824:4194304`.

**t2-r822, the base with `tcp_limit_output_bytes` 512 KiB: 59.58 GB/s.**
Below TCP 0.56 MB a socket (2.75 at 4 MiB), TCP's RTT 0.49 ms (1.71),
the Send-Q still 3.56 MB: the bytes moved up into TCP, unsent, against
the 4 MiB send buffer.  Memory 136.2 GB/s (2.29 a byte), cycles a KiB
500 / 282 / 782.  **The bound stays in the base** (Mike: "have you kept
the change that allowed for the correct movement of the bottleneck? ...
We need to ensure that the bottlenecks get pinned to their new location
as we move them"): it moves the queue out of the qdiscs and the NIC into
TCP, where the next change, admission by send space, takes it.  The
value is pinned by a sweep: 256 KiB (r823) and 4 MiB (r824) running,
then 768 KiB (r825) and 1 MiB (r826).
