# Round 91: who on the client sends from the far node

tardis2, build 522 (7.3.3-14.hs.522.canary, 16 KiB), loan mode on two
ports, the settled setting with the pool's stack at 96 (round 90).
Client 106: 7.3.0-0.rc7.2.hs.469.el9.x86_64, two nodes (CPUs 0-23 near
port a's `ens3np0`, 24-47 near port b's `ens6np0`).

**Why.**  `twoport.sh` reports the share of each client port's packets
sent from the far node's CPUs (from the client NIC's per-queue counters
and its XPS map).  It runs 1.4% to 2.9% and varies run to run
(`t2-r382..r387`), though fio is pinned to the near node
(`cpus_allowed`) and every run sets the client's
`workqueue.default_affinity_scope` to `numa` (`CLI_SCOPE`).  Mike's
idea for round 91 is controls in the client's sunrpc that hold its
workers to a port's CPUs.  The suspicion: `rpciod` and `xprtiod` are
unbound workqueues without `WQ_SYSFS`, so their scope cannot be made
strict and the scheduler may move their workers to the other node.
Before any code, this round finds who the far senders are.

**How (`run388.sh`).**  Two runs as round 90's (`WARM=10 RUNTIME=40
WRITES=128`), `t2-r388` and `t2-r389`, no perf on the server.  When the
window opens the client, for 30 s, counts every skb handed to each port's
driver (`net:net_dev_start_xmit`, filtered on the device) on each node's
CPUs (`cli-st-n0.txt`, `cli-st-n1.txt`), and records one in five of those
sent from the far node's CPUs with kernel call chains
(`cli-far-a.script.xz`, `cli-far-b.script.xz`).  perf on the client,
which has no bpftrace.  The tracepoint counts skbs (a TSO skb is many
packets), so its share is not the NIC's; the NIC's figure is in
`pmu.log` beside it.

**The first pair, `t2-r388` and `t2-r389`, did not trace the window.**
`common.sh` sets `D` (port a's device), so `run388.sh`'s default of 30
for it was never taken and the client's perf ran `sleep enp161s0f0`,
which fails at once: its counts and samples cover only perf's own start
and stop.  What they caught, about 1,500 far samples, was 92% to 100%
`rpciod` (`rpc_async_schedule` above `__tcp_transmit_skb`), the rest
`xprtiod`.  The variables are now `FAR_C` and `FAR_SECS`, and the pair is
run again as `t2-r390` and `t2-r391`.  Their rates: 69.17 and 69.63
GB/s; the NIC's far share 3.0%/2.9% and 3.4%/3.6% (a/b).

The client NIC's own counters in `t2-r388` show its queues used
unevenly too: of port a's 64 queues most carry about 0.8M packets in the
window, a few 3.3M and one (`tx9`) 12.5M; port b alike (`tx19` 10.3M,
`tx3` 7.9M).
