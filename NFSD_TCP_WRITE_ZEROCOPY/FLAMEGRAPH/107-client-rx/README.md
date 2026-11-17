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

**`client-read.py` no longer counts iowait as busy** (Mike: "fix
client-read.py to not count iowait as busy").  Its busy was 100 less
%idle, so io_uring's wait for completions, 9.38% iowait in t2-r863
where libaio's t2-r862 had none, read as 93% busy for the same work
(sys, irq, soft and usr 83.6% against 83.0%).  Busy is now 100 less
%idle and %iowait, with iowait given beside it; t2-r863's summary was
written again with it (84% and 83%).  Summaries before t2-r863 had no
iowait to speak of and are unchanged by it.

**The A/B: io_uring with registered buffers reads 80.23 GB/s**, the
first NFS READ on the rig above 80.  New kernel warnings 0 in each.

| run | engine | GB/s | client busy | client memory | a byte | memcpy | pinning | submission | iou-wrk |
|---|---|---|---|---|---|---|---|---|---|
| t2-r862 | libaio | 78.73 | 84% / 82% | 335.5 GB/s | 4.26 | 32.01% | 2.61% | 11.45% | 0 |
| t2-r863 | io_uring | 78.75 | 84% / 83% (iowait 9.4%) | 335.6 GB/s | 4.26 | 32.17% | 2.61% | 11.21% | 0 |
| t2-r864 | io_uring, `fixedbufs=1` | **80.23** | 83% / 81% | 338.7 GB/s | 4.22 | 32.72% | under 0.2% | **8.55%** | 0 |
| t2-r865 | libaio | 78.61 | 84% / 84% | 340.7 GB/s | 4.33 | 32.47% | 2.62% | 11.45% | 0 |

(t2-r863's row as printed read 93% busy, counting its iowait; the
figure here is the corrected one.)

- **Plain io_uring changes nothing** but where the wait is counted:
  the same rate, copy, pinning and submission as libaio, no io_uring
  workers (NFS's direct path takes the nonblocking submit).
- **Registered buffers take the pinning out** (`try_grab_folio_fast`,
  2.61%, falls under the profile's floor) and **a quarter of the
  submission** (11.45% to 8.55% of the client's cycles), and the rate
  rises 1.9% with the client a little less busy.  The two libaio runs
  either side are 0.12 GB/s apart.
- **The copy is as it was** (4.22 to 4.33 bytes of the client's memory
  a byte), and it is still the limit: a third of the client's cycles.
- One run of each.  Using it for the passes would change their shape,
  which is Mike's to decide; the passes stay on libaio until then.

About to run (Mike: "re-run io_uring with fixedbufs"): `ab-engine.sh
866:io_uring:fixed` through `step.sh /var/tmp/zc-run-logs/r107 engine2`.

**t2-r866, io_uring with registered buffers again: 80.26 GB/s**
(port a 40.15, port b 40.11), client busy 82% and 83%, client memory
341.7 GB/s at 4.26 a byte, memcpy 33.24%, pinning under 0.2%,
submission 8.68%, no io_uring workers, new kernel warnings 0.  Within
0.03 GB/s of t2-r864: the gain repeats.  The passes stay on libaio
until Mike decides.  Round 106 and 107's story is on the claude.ai page
"NFSD Read Promised Count" (https://claude.ai/artifact/B7V1qW3uD5qGwX9DAhGDkz).

**The READ pass with io_uring and registered buffers** (Mike: "run the
READ pass with io_uring fixedbufs").  About to run: `IOENGINE=io_uring
FIXEDBUFS=1 ./read-check.sh 867` through `step.sh
/var/tmp/zc-run-logs/r107 rcuring`, at the settled setting, the pass's
own shape (`WARM=10 RUNTIME=40`, a trim and 120 s of rest), set beside
t2-r859's 79.24 on libaio.
