# Round 68: whether S+H's rate is real, and where the client sends from

Round 66's two S+H runs (71.21 and 71.37 GB/s) were the highest of 16
since round 64.  [run259.sh](run259.sh) stopped after its first run
(`t2-r259`, not read) when tardis2's root filled with perf data, so
[run273.sh](run273.sh) writes `perf record` to `/dev/shm`, keeps only
`pmu.script` on root, and stops if root has under 1 GB free.  B and
round 66's SH alternate four times (`t2-r273..r280`).  In the last four
runs, two of each, the client runs [sendprobe.sh](sendprobe.sh) for
10 s inside the window: `net:net_dev_start_xmit` with call chains, every
50th transmit, classed by the NIC, the node of the CPU that sent it,
and the path that sent it.  No new kernel warning in any run, the stock
stack put back, the setting audited ([run273.out](run273.out)).

| run | | probe | GB/s | nfsd | `__lwq_dequeue` | `svc_recv` | far |
|---|---|---|---|---|---|---|---|
| `t2-r273` | B | | 71.33 | 248.8k | 435 | 4505 | 1.2% |
| `t2-r275` | B | | 69.87 | 242.8k | 390 | 3918 | 2.7% |
| `t2-r274` | SH | | 71.09 | 250.3k | 1165 | 3395 | 1.6% |
| `t2-r276` | SH | | 70.28 | 245.6k | 1004 | 3052 | 2.4% |
| `t2-r277` | B | yes | 69.54 | 224.2k | 361 | 3665 | 1.2% |
| `t2-r279` | B | yes | 69.89 | 223.9k | 355 | 3537 | 1.3% |
| `t2-r278` | SH | yes | 70.65 | 227.1k | 681 | 2640 | 1.8% |
| `t2-r280` | SH | yes | 70.68 | 225.4k | 630 | 2553 | 1.4% |

- **Without the probe B and SH carry the same:** 70.60 and 70.69 GB/s
  on average.
- **With it SH was about 1 GB/s ahead in both pairs** (70.67 against
  69.72), at far shares no lower than B's.  With the client's CPUs
  loaded by the probe, SH holds the rate where B loses it.  Two pairs
  show a hint, not a result.
- **The probed runs count about 22k fewer nfsd cycles a WRITE, B and SH
  alike.**  Not explained; probed runs are read only against each
  other.
- The probe's `perf` took the client longer than its 10 s (the probed
  runs took 10 minutes, not 5).

## Where the client sends from

The client has `ntuple` and aRFS off, no RPS, and each NIC's 48
completion interrupts on its own node.  `t2-r277` classed every
transmit as reached from `tcp_rcv_established` (an ACK) and found the
far node's 2% there; `sendprobe.sh` was then split into the receive
softirq (`net_rx_action`) and the socket's backlog (`__release_sock`),
with the far node's samples by task.  `t2-r278` to `t2-r280`, shares of
each NIC's transmits:

| | softirq | backlog | `sendmsg` | far node (all backlog) |
|---|---|---|---|---|
| `t2-r278` SH | 57.9% | 39.5% | 0.1% | 2.4% |
| `t2-r279` B | 56.4% | 40.1% | 0.2% | 3.2% |
| `t2-r280` SH | 58.2% | 39.0% | 0.2% | 2.5% |

About 40% of the client's sends are an ACK that arrived while a task
held the socket, processed when it let go, and the far node's sends are
nearly all of these, in the client's unbound `kworker/u206` workers
(rpciod and xprtiod) when they run on the far node.  So the far share
is where the client's kworkers are scheduled, not the server's doing
and not the interrupts.  Keeping those workers on each NIC's node would
take it out of the comparison.
