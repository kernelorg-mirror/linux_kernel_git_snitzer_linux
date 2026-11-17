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

**The sweep (t2-r822 to r825): the bound changes where the bytes wait,
not the rate.**

| run | `tcp_limit_output_bytes` | GB/s | below TCP a socket | Send-Q | TCP RTT |
|---|---|---|---|---|---|
| `t2-r823` | 256 KiB | 59.40 | 0.30 MB | 3.57 MB | 0.32 ms |
| `t2-r822` | 512 KiB | 59.58 | 0.56 MB | 3.56 MB | 0.49 ms |
| `t2-r825` | 768 KiB | 59.52 | 0.80 MB | 3.54 MB | 0.62 ms |
| `t2-r824` | 4 MiB | 59.34 | 2.81 MB | 3.50 MB | 1.70 ms |

Memory 2.29 to 2.30 bytes a byte in all.  **256 KiB is in the base**
(Mike: "stop and just use 256K"; `prof.sh` and `ab.sh` set it): the least
below TCP and the shortest RTT for the same rate.  1 MiB (r826) was
stopped by hand after its setup and ran its load out to a clean
teardown; its figure is not counted.  The Send-Q stays at 3.5 MB a
socket at every bound: the bytes wait in TCP, so the next change bounds
what a connection takes on, not what TCP sends.

## Admission by send space

`f379dad69c793` "SUNRPC: Bound the replies a run-to-completion
connection holds": `sunrpc.svc_rtc_send_max`, the bytes a connection's
TCP send queue may hold before its work item takes another call.  Past
it the call stays unread in the socket, where it holds no pages, and the
socket is marked `SOCK_NOSPACE`, so the ACKs that shrink the queue call
`svc_write_space()`, whose enqueue kicks the work item to receive again.
`rtc_stats` counts the receives it held back (`send_full`).  For 534,
on top of the placement fix: `sunrpc.ko` `A00D335DAB7B990CCA5645B` in
`/var/tmp/zc-run-logs/sunrpc-sendmax-534/` (every export the same).
[ab.sh](ab.sh) takes it as a third field and puts it back to 0 after.

About to run, through `step.sh`: the module swapped in with no client
connected, `settled.sh apply`, then `ab.sh 827:262144:0
828:262144:1048576 829:262144:524288`.

**t2-r827, admission off: 59.60 GB/s**, memory 2.28 bytes a byte, below
TCP 0.30 MB a socket, Send-Q 3.52 MB, RTT 0.32 ms: the base as before.

**t2-r828, admission at 1 MiB: every connection hung, and the host with
it.**  Each of the 32 connections hit the bound once (`send_full` 32)
and none ran again: their send queues drained to 0 while 2.3 MB of calls
waited unread, `runs` did not move with the bound set back to 0, and the
run hung in its load from 05:07.  The cause: a work item owns its
transport (`XPT_BUSY`, taken in `svc_xprt_enqueue()` before
`svc_rtc_divert()`) until its receive releases it.  `f379dad69c793`
skipped the receive and so never released it, and with `XPT_BUSY` held
every later `svc_xprt_enqueue()` (new data, write space, close) returns
at its test-and-set.  `systemctl restart nfs-server` (05:23:57) then
stuck in `svc_xprt_destroy_all()`: shutdown closes a transport by
enqueuing it, which the same bit refuses, so `nfsdctl threads 0` sleeps
in its retry loop, unkillable.  Only a reboot clears it.  The client's
fio and 32 hard mounts wait on the server.

`f379dad69c793` is not to run again as it is.  The fix: holding a
receive back must release the transport (clear `XPT_BUSY` without
enqueuing) and let the write-space callback's enqueue take it again,
with a KUnit case for a held-back receive resumed by write space before
it goes back on the rig.
