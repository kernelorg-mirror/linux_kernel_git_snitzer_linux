# Round 138: the client's RPC iostats per CPU

Mike, 18 November: "yes, make the RPC iostats per-CPU", from round
137's profile: `rpc_count_iostats_metrics()` 2.17% of the client's
samples and the spin lock it takes 0.52%, about 0.36 us of the
client's CPU a READ at 3.59 M IOPS, with the client the limit.

The change, two code commits (W=1 clean on the client; no sparse
there):

- `7e85409ea83f6` "NFS: Name a flexfiles op's RPC iostats by offset,
  not by element": `cl_metrics + op` for `&cl_metrics[op]`, no change
  in behaviour.
- `772a7f50696af` "SUNRPC: Count a client's per-procedure iostats per
  CPU": `cl_metrics` is a per-CPU array; a task adds to its CPU's copy
  with preemption off in a `u64_stats_sync` section, no lock;
  `rpc_clnt_show_stats()` sums the CPUs.  The cost is memory: about 72
  bytes a procedure a CPU, so an NFSv3 client (22 procedures) holds
  about 76 KiB on 48 CPUs where it held about 2.8 KiB, and NFSv4's
  procedure table is about three times longer.

`ab-iostats.sh`: B (the `updates/` stack) against P, 4 KiB random READs
at iodepth 96 B P B P (t2-r1129 to r1132), then the WRITE and READ
passes on P (r1133, r1134).

## The A/B (23:38 to 00:02)

| Run | Arm | 4 KiB IOPS, iodepth 96 | p99 a / b (ms) | CPU a READ, server / client (us) | Client busy (busiest) |
|---|---|---|---|---|---|
| t2-r1129 | B | 3.42 M | 4.82 / 4.08 | 15.72 / 11.79 | 84% (99%) |
| t2-r1130 | P | 3.49 M | 3.26 / 3.39 | 16.14 / 11.83 | 86% (93%) |
| t2-r1131 | B | 3.52 M | 3.49 / 3.33 | 16.00 / 11.73 | 86% (99%) |
| t2-r1132 | P | 3.59 M | 3.23 / 3.42 | 15.69 / 11.63 | 87% (91%) |
| mean | B | 3.47 M | | 15.86 / 11.76 | |
| mean | P | 3.54 M (+2.0%) | | 15.92 / 11.73 | |

P is 2% ahead, inside B's own spread (3.42 to 3.63 M on this stack),
with a shorter p99 in both runs and the client's busiest CPU at 91 to
93% against 99% in both of B's; the client's CPU a READ does not move
(11.73 against 11.76 us), not the 0.36 us round 137 pointed at.

On P: the WRITE pass t2-r1133 **77.94 GB/s PASS**; the READ pass
t2-r1134 80.16 GB/s, **FAIL on one new kernel message**, the server's
NIC firmware mailbox on port b at 00:00:26 ("RM message [a4:00.0] send
timed out", "XSL_MSG_GET_LINK_CONFIG failed, error: 110"), the third
in this boot's 25 hours; nothing on the client.  tardis2's EFI
partition on nvme8 also logs "FAT-fs (nvme8n1p1): Volume was not
properly unmounted" every few minutes (23:33, 23:40, 23:48, 23:59),
which the pass does not count.  The client back on `updates/` and the
settled setting after.  Round 138 needs a READ pass that passes.

## P's READ pass again, and P's profile (00:13 to 00:24)

- t2-r1135, the READ pass on P: **82.32 GB/s PASS**, no new kernel
  warning (B read 82.38 at t2-r1123).
- t2-r1136, round 137's profile on P (`prof-both137.sh`): 3.52 M IOPS
  (1.74 / 1.78 M), p99 4.36 / 3.52 ms, 15.64 / 11.73 us of CPU a READ.
  P's modules were loaded from `/root/zc-ckern/IOS`, which perf does
  not look in, so `cli-self.txt` and `cli-children.txt` were made again
  with `--symfs` holding P's modules and `--kallsyms=/proc/kallsyms`.

| Client function (self) | t2-r1128 (B) | t2-r1136 (P) |
|---|---|---|
| `rpc_count_iostats_metrics` | 2.17% | **1.61%** |
| `_raw_spin_lock` | 6.55% | 6.04% |
| `native_queued_spin_lock_slowpath` | 2.25% | 2.29% |
| `xprt_request_enqueue_receive` | 2.05% | 2.04% |
| `nfs_fattr_init` | 1.75% | 1.69% |
| `rpcauth_refreshcred` | 1.62% | 1.23% |

The lock is gone, but most of the function's time is not: 1.61% of the
client's samples stay in it.  `objdump` of P's `sunrpc.ko` puts its hot
spot (0x59c82, `perf annotate` disassembled another copy of the module
and is not to be trusted here) right after `movslq 0x1d8(%r12),%rdx`,
the load of `req->rq_ntrans`: the first read of the task's `rpc_rqst`
in the completion, a cache line last written on another CPU (the
receive).  So what round 137 counted against the iostats was mostly
the completing CPU's first touch of the request, which moves to the
next reader if the counting goes; the counters and their lock were
about 0.56% (0.08 us a READ).  That fits the A/B: no change in the
client's CPU a READ, a busiest CPU a few points lower.
