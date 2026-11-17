# FLAMEGRAPH round 107: the client's receive path

Mike, 17 November: "profile the client's receive path".  Round 106
settled the 1.5 MiB bound (READ pass 79.24 GB/s, t2-r859), and its
profile at the new setting (t2-r860) moved the limit to the client:
its CPUs 83 to 84% busy (busiest 95%), and a reply waits 0.98 ms in
its sockets' receive queues against 0.17 ms in the server's send
queues.  The server's CPUs are mostly idle.

## What t2-r860 already shows of the client

Its 5 s client profile at 499 Hz (`cli-perf-children.txt`), as shares
of all the client's cycles:

| path | share |
|---|---|
| the RPC receive worker, `xs_stream_data_receive_workfn` | 51.2% |
| ... copying skbs into fio's pages, `skb_copy_datagram_iter` | 44.6% |
| ... `memcpy` itself | 40.2% |
| the NIC's receive softirq, `mlx5e_napi_poll` | 11.7% |
| fio issuing READs, `io_submit` to `nfs_direct_read_schedule_iovec` | 11.6% |
| completing READs, `nfs_direct_read_completion` | 10.3% |
| idle | 17.6% |

The client is two Xeon Gold 6542Y sockets, 24 cores each without
SMT, 60 MiB of L3 a socket.  Port a (`ens3np0`) is on node 0 with its
interrupts on CPUs 0 to 23, port b (`ens6np0`) on node 1 on 24 to 47;
fio is pinned to each port's node (`PIN=1`), and rpciod and xprtiod
keep to their node.  A node's sockets hold about 38 MB unread (16 at
2.40 MB), much more than the L3's ways DDIO writes into, so the
question is whether the copy reads each byte back from DRAM.

## The plan

`client-prof.sh 861`, through `xeu-rig/step.sh
/var/tmp/zc-run-logs/r107`: the READ strobe at the settled setting,
files kept, with on the client from the load's second 6 five seconds
of per-socket counters (memory reads and writes, UPI data, inbound I/O
hitting and missing the L3, cycles, instructions, L3-missing loads by
where they were served), from second 12 eight seconds of call chains
at 999 Hz, and `client-read.sh`'s own profile from second 21.  Nothing
on the client is changed.
