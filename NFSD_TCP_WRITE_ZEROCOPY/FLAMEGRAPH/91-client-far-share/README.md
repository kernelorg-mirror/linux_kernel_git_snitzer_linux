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

## Result (`t2-r390`, `t2-r391`)

| run | GB/s | port | skbs in 30 s | from the far node | NIC's far share |
|---|---|---|---|---|---|
| t2-r390 | 70.44 | a | 8,807,408 | 197,331 (2.24%) | 2.3% |
| | | b | 9,305,152 | 219,917 (2.36%) | 2.5% |
| t2-r391 | 71.59 | a | 8,821,740 | 131,726 (1.49%) | 1.5% |
| | | b | 8,822,203 | 183,811 (2.08%) | 2.2% |

- **The NIC's far share is the sending CPU's.**  Counted by the CPU that
  handed each skb to the driver, the share is within 0.1 of what
  `twoport.sh` reads from the queues, so a socket's cached queue does
  not blur it.
- **Who: `rpciod`, 97.4% to 97.8% of the far sends in each port and
  run**, `rpc_async_schedule` above `__tcp_transmit_skb`; `xprtiod` 2.2%
  to 2.5%; TCP's timers, syscalls and the rest under 0.1%.  fio itself
  (pinned) sends nothing from the far node.  The far sends are spread
  over every CPU of the far node, 2.5k to 11k samples each
  (`farread-r390-r391.txt`).
- **Why it can happen:** the client's sunrpc (`v7.3.0-0.rc7.2.hs.469.el9`,
  `net/sunrpc/sched.c`) makes both with `alloc_workqueue(...,
  WQ_UNBOUND | WQ_MEM_RECLAIM, 0)`.  The run's `numa` scope queues an
  async task's work to the pod of the CPU that queued it, but an unbound
  workqueue's scope is not strict by default, so the scheduler may run
  its worker on the other node; and without `WQ_SYSFS` neither has
  `affinity_strict` (nor `affinity_scope` or `cpumask`) in
  `/sys/devices/virtual/workqueue/` to change that.

**Next (for Mike):** a client `sunrpc.ko` from that tag with `WQ_SYSFS`
on `rpciod` and `xprtiod` (nothing else), so a run can set each to
`numa` and `affinity_strict=1`; then runs against the stock module, the
far share and the rate.

## The A/B: rpciod and xprtiod strict to a node (`run392.sh`)

Built on the client against its `kernel-devel`, with its own compiler,
from `v7.3.0-0.rc7.2.hs.469.el9`'s `net/sunrpc` (`CONFIG_SUNRPC_GSS` and
`_XPRT_RDMA` left out of the make, their modules not swapped): A as it
is, B with `wqsysfs.diff` (`WQ_SYSFS` on `rpciod` and `xprtiod`, nothing
else).  A against the rpm's `sunrpc.ko`: the same 2,487 symbols, the same
parameters, data and bss, text 199 bytes longer (gcc differs from the
rpm's); srcversion differs, as an `M=` build's does.  Both arms are these
builds, so the compiler is the same on both sides; stock goes back at the
end.  `cswap.sh` swaps them on the client with nothing mounted: it stops
the client's own NFS server and helpers (it exports `/mnt/nvme0n1`,
`/mnt/nvme1n1` and volumes for other work; no client was connected),
unloads every module on sunrpc, loads the build by path with `insmod`
(nothing installed: a reboot brings stock back), and starts them again.
In B, `affinity_strict` is 1 on both workqueues, their scope `default`,
which `twoport.sh` sets to `numa` for each run.  Runs A B B A A B,
`t2-r392..r397`, as round 90's, no perf.
