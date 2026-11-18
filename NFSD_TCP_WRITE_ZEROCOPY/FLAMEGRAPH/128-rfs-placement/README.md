# Round 128: where RFS puts an NFS socket's receive

Mike, 18 November: "look at the RFS placement, profile only".  No code.

From round 127's profile (t2-r1045, 2.91 M), us a READ:

| | Ring CPUs | Work CPUs |
|---|---|---|
| `process_backlog` (receive steered by RPS/RFS) | 0.43 | 2.22 |
| - of it `tcp_v4_rcv` | 0.34 | 1.81 |
| `svc_tcp_recvfrom` (the call read by the work item) | 0.43 | 1.51 |
| `tcp_add_backlog`, `__release_sock` (socket owned on arrival) | 0.11 | 0.33 |
| the card's receive poll | 0.53 | 0 |
| steering a packet (`enqueue_to_backlog`, `get_rps_cpu`) | 0.14 | 0 |
| RPS IPIs sent | 0.12 | 0 |

`rps_cpus` is 8-55 on every queue (4 a port), `rps_sock_flow_entries`
32768, `rps_flow_cnt` 4096.  The spill puts connections on ring CPUs
too, and their receive lands there as well (0.43 against 0.43): RFS
follows the reading CPU, not `rps_cpus`.

Whether each socket's receive runs on the CPU that reads it is not in a
profile: `rfs-map.sh 1046` runs nfs4k under `settled-iops.sh` with
`rfs-map.bt` for 5 s (kprobes on `tcp_v4_do_rcv` and on
`tcp_read_sock`/`tcp_recvmsg`, port 2049 only), and `rfs-map.py`
gives the share of packets received on the socket's reading CPU.

## Result: t2-r1046 (18 November, 09:38 to 09:43)

2.86 M IOPS with the probe on for 5 s (1.45 / 1.41), no new kernel
warning.  Both bpftrace maps filled at 4096 keys, so (socket, CPU) pairs
seen late are missing: the small off-home counts are undercounted.

- 96 sockets, all seen in both maps; 16 of them read mostly on a ring
  CPU (the spill).
- **99.0% of 17.9 M packets were received on the CPU that reads the
  socket most; 95.8% of 31.2 M reads were on that CPU.**
- Every socket was read on 36 to 49 CPUs in the 5 s and received on
  about as many, a few reads or packets on each.

The other reads are nfsd threads': 4.7% of `svc_tcp_recvfrom`'s samples
in t2-r1045 are in `nfsd`, 94.0% in the rtc kworkers.  The rtc work
item hands a connection to a thread when it cannot peek the next call's
header (`fallback_no_peek`, "call header not peekable (partial, TLS)",
74.8 M since boot against 724 M calls offered: partly WRITE runs).  A
thread reads on its own CPU, RFS records that CPU as the flow's, and the
next packets go there until the work item reads again: the 1% received
off home.

So RFS steers right: the receive is on the reading CPU for 99% of
packets, and moving it would not save the 2.6 us of TCP receive.  What
the probe points at is the hand-off to threads on a partial header: a
thread's wakeup and switch for a call the work item could serve once
the rest arrives (`finish_task_switch` 0.21 us a READ in t2-r1045),
and the flow's receive steered away and back.  How many READ calls take
it in an IOPS run is not measured yet (rtc_stats before and after one).

## The no_peek fallbacks in one IOPS run (Mike: "measure the no_peek fallbacks in one IOPS run")

`/sys/kernel/debug/nfsd/rtc_stats` read before and after one
`settled-iops.sh` run of nfs4k.sh, t2-r1047; the counts' differences
against the run's calls offered (`read_offered`) and diversions.

### Result: t2-r1047 (09:48 to 09:52)

2.92 M IOPS (1.49 / 1.43), p99 2.80 / 3.00 ms, no new kernel warning.
`rtc_stats` over the run (about 25 s of load):

| Count | In the run | Share |
|---|---|---|
| `diverted` (the work item classified a call) | 80,218,983 | |
| `read_offered` (a READ served on the fast path) | 72,485,841 | 90.4% of diverted |
| `fast_completed` | 72,484,994 | every READ offered |
| **`fallback_no_peek`** (header not peekable, to a thread) | **7,733,021** | **9.6% of diverted; 10.7 a 100 READs** |
| `fallback_conn_full`, `not_fast`, `file_miss`, `pool_empty` | 968 | |
| `undiverted` | 10,476,278 | |
| `rebalanced` | 27 | |

Every READ was still served on the fast path (`fast_completed` is the
READs at 2.92 M for about 25 s), so a `no_peek` hand-off serves no
call: an nfsd thread is woken, finds the record still partial, and the
connection comes back to its work item when the rest arrives.  About
310,000 thread wakeups a second for nothing.

What they cost, from t2-r1045's profile: the `nfsd` threads hold 2.33%
of the server's samples, about **0.5 us a READ** (`finish_task_switch`
0.48% of it), not counting the waking side or the RFS flow steered to
the thread's CPU and back.
