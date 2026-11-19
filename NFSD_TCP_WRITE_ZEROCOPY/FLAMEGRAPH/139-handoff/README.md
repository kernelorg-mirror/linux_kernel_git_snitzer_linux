# Round 139: the cross-CPU handoff of a 4 KiB READ on the client

Mike, 19 November: "rebase ... so that 7e85409ea83f6 and 772a7f50696af
are just dropped, then measure the cross-CPU handoff".  Round 138
found that most of `rpc_count_iostats_metrics()`'s time was the
completing CPU's first read of the task's `rpc_rqst`, a line last
written by the CPU that received the reply.  Done first: round 138's
two code commits dropped from the history (its four docs commits
replayed on `1d05658995801`; the tip before is
`kernel-7.3/canary.pre-drop-iostats`).

The client: 48 CPUs, two sockets of 24 cores (0-23, 24-47), no SMT, one
L3 of 60 MiB a socket.  `handoff.sh 1137`: one 4 KiB random READ run at
iodepth 96 on the client's `updates/` stack and, from its eighth
second, 0.3 s of the client's sunrpc tracepoints with CPU and time
(`rpc_task_begin`, `xprt_transmit`, `xprt_lookup_rqst`,
`rpc_stats_latency`, `rpc_task_end`).  `handoff.awk` follows each task
from its submit through its xid to its completion and counts, for each
step, same CPU, same socket or the other socket, with the time between.

## The trace (t2-r1137)

3.48 M IOPS (1.73 / 1.75 M), completion mean 1.54 / 1.52 ms, no new
kernel warning; 0.3 s traced from the eighth second, 989,571 READs
followed from submit to completion (the trace's 542 MB kept in the
client's `/dev/shm/zc-h139-1137.data`).

| Step | Same CPU | Another CPU, same socket | The other socket | Mean time |
|---|---|---|---|---|
| submit -> send | 76.3% | 23.7% | 0 | 228 us (same CPU), 334 us (other) |
| send -> reply matched | 3.0% | 97.0% | 0 | 868 us |
| reply matched -> completion | 100.0% | (a handful, an xid matched to the wrong task) | 0 | **183 us** |
| submit -> completion | 3.2% | 96.8% | 0 | 1.35 ms |

- **Nothing crosses the sockets.**  Each port's fio jobs, transports
  and receive work stay on one socket's CPUs, so every handoff is
  within one L3.
- **A READ is sent on the CPU that submitted it (76%), and its reply is
  matched on another (97%)**: the transport's receive worker, which
  then completes it on that same CPU.  So the request's lines written
  at the send are read on another CPU at the match and the completion
  (round 138's miss: an L3 hit, not memory), and fio, on the
  submitting CPU, reaps a completion done elsewhere.
- **183 us from the match to the completion, on the same CPU**: the
  woken task waits in that CPU's rpciod queue behind the receive work
  of the same CPU.  That is 14% of the READ's 1.35 ms, and all of it
  queueing on a busy CPU.
- 228 to 334 us from submit to send: the task waits for its transport
  (its slot and the send lock).
