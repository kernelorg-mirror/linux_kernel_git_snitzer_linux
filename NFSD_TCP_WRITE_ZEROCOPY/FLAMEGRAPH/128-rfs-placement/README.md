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
