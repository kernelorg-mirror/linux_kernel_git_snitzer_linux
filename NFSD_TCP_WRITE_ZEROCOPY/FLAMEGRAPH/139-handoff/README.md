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
