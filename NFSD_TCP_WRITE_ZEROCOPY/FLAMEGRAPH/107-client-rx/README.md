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

## t2-r861: the client copies from DRAM into DRAM

78.05 GB/s under the profilers, new kernel warnings 0.  Per client
socket over 5 s (`cli107-stat.txt`), each socket receiving about 39
GB/s:

| | socket 0, port a | socket 1, port b | a byte received |
|---|---|---|---|
| memory reads (`cas_count_read`) | 85.8 GB/s | 88.4 GB/s | about 2.2 |
| memory writes (`cas_count_write`) | 81.0 GB/s | 81.4 GB/s | about 2.1 |
| inbound I/O requests missing the L3 (`io_miss` of `io_hit + io_miss`) | 46% | 46% | |
| L3-missing loads from local DRAM | 97.8% | 98.1% | |
| instructions a cycle | 0.55 | 0.55 | |

- **About 4.3 bytes of the client's memory traffic for each byte
  received**, what a copy costs with both ends outside the cache: the
  NIC's write to DRAM (1), the copy's read of it (1), and the
  destination, fio's buffer, read for ownership (1) and written back
  (1).
- The source: a node's sockets hold about 38 MB unread (16 at 2.40
  MB), and DDIO writes into a small part of the 60 MiB L3; 46% of the
  NIC's inbound requests miss.  The destination: fio's buffers are 16
  jobs × iodepth 16 × 1 MiB, 256 MiB a port.
- Not NUMA: 98% of L3-missing loads are local, UPI data traffic is
  small, and `memcpy`'s samples split 15.97% and 16.29% between the
  nodes.  (Those load counters do not see `rep movsb`'s loads; the
  copy shows only in the memory controllers' counts.)
- `memcpy` is 32.28% of the client's cycles (`cli107-self.txt`), all
  from `_copy_to_iter` in the RPC receive worker's `tcp_recvmsg`: about
  5 GB/s a core, the copy waiting on memory.
- Against the frame: iperf3 alone received 98.31 GB/s on this client,
  copying too, but into a small buffer it reuses, its destination in
  the cache.  NFS O_DIRECT copies into fio's 256 MiB a port.

## io_uring at the client (Mike: "please consider using io_uring at the fio client")

It leaves the copy as it is.  Registered buffers (`fixedbufs=1`) hand
NFS pages already pinned, which removes `try_grab_folio_fast` (2.61%),
and io_uring reaps completions without a system call each batch.  If
NFS's direct path does not take io_uring's nonblocking submit, each
READ goes to io_uring's own workers, which may cost more.  To run:
`twoport.sh`'s new `IOENGINE` (`libaio`, the default, so the passes
keep their shape) and `FIXEDBUFS`, in three strobes at the settled
setting with this round's client counters: libaio, io_uring, and
io_uring with `fixedbufs=1`.

`twoport.sh` takes `IOENGINE` and `FIXEDBUFS` (the default job text
unchanged, checked by checksum against the old script, and the fill
always on `libaio`, so the kept files' markers hold).  About to run,
through `step.sh /var/tmp/zc-run-logs/r107 engine`: `ab-engine.sh
862:libaio 863:io_uring 864:io_uring:fixed 865:libaio`, libaio on
either side for the spread.
