# Round 155: where the rtc balance puts the connections

Mike, 19 November: "wasn't spill introduced purely for IOPS?  shouldn't
it be disabled for all now?  but yes: do add that snapshot and run it
with no spill as round 155".

**The spill is gone from the IOPS setting.**  `settled.sh` never held
it (none, weight 2), so the WRITE and READ passes ran without it; only
`settled-iops.sh` added it (round 116), and round 154 found it costs
1.05 us of the server's CPU a READ at the same IOPS.  `settled-iops.sh`
now leaves both at `settled.sh`'s.

**The question.**  Round 154's t2-r1214 (no spill) ran 3.29 M against
t2-r1212's 3.64 M, steadily from its second second, with both hosts
less busy and ten of nfsd's CPUs (8-55) at 69 to 83% while others were
full.  Each connection's work runs on the CPU the rtc balance places it
on (RFS takes its frames there), 96 connections on 48 CPUs, rebalanced
every 250 ms: an uneven placement would leave some CPUs full and their
connections' jobs slow, others idle.

`placement.sh 1215 1216`: two IOPS runs (round 151's `packets.sh`),
and through each, every second, nfsd's `rtc_stats` (debugfs: `placed
cpu:n`, each CPU's connections, and `rebalanced`).  `placement.awk`
gives each second's spread over nfsd's CPUs and, over the mpstat
window, CPUs grouped by their mean connections beside their busy.

**The spill's code is gone at the tip (Mike, 19 November: "please revert
all code changes associated with the spill at the tip of the tree"):**
`143e3df791c88`, "SUNRPC: Drop the rtc balance's spill onto other CPUs",
reverts `219a15abdd219` and the rebalancer's use of the spill CPUs from
`4c084f9754fe1`; `svc_xprt.o` builds at W=1 with no warning.  tardis2's
sunrpc still has the two parameters, at none and weight 2, which is the
same as not having them; this round runs on it.

## Result: an even spread when every connection lands, four that did not

`placement.sh 1215 1216`, then `1217`.  t2-r1215 failed at its start
(no load, exit 1): `settled.sh` was rewritten while the run used it
("bad interpreter: Text file busy").  t2-r1217 replaces it.

| run | IOPS | a / b M | p99 ms a / b | server busy | server us a READ | client us a READ | placed on 8-55, steady | CPUs with 1 / 2 | busy, 1 / 2 connections | rebalanced |
|---|---|---|---|---|---|---|---|---|---|---|
| `t2-r1216` | 3.49 M | 1.78 / 1.71 | 3.33 / 3.98 | 80% | 14.67 | 11.28 | 96 (+4 off) | 0 / 48 | - / 98% | 2 |
| `t2-r1217` | 3.44 M | 1.83 / 1.61 | 3.13 / 5.60 | 80% | 14.88 | 11.44 | 92 (+3 to 4 off) | 4 / 44 | 69% / 98% | 4 |

- **When every connection lands, the spread is exact:** from about the
  15th second, t2-r1216 held two connections on each of nfsd's 48 CPUs,
  after two moves; all 48 ran 98% busy.
- **In t2-r1217 four connections were never placed on nfsd's CPUs** for
  the whole run (96 placed in all against t2-r1216's 100).  Four CPUs
  (8, 45, 46, 47) held one connection and ran 69% busy; the four
  connections' work ran some other way, and port b fell behind.  The
  rebalancer moves placed connections only, so it cannot help.
- **Some calls do not take the rtc path at all:** since boot, `rtc_stats`
  counts 1.6 billion undiverted against 7.0 billion diverted, and
  `fallback_partial` (244 million) is by far the largest fallback.  Per
  run, and which connections, is round 156's to find.
- **No spill, three runs (t2-r1212, r1216, r1217):** 14.42 to 14.88 us of
  server CPU a READ, against 15.31 to 16.18 with it (round 154).

# Round 156: which connections miss the rtc path, and why

Mike, 19 November: "yes, add that to placement.sh and run round 156".
`placement.sh` now also copies `rtc_stats` whole at the load's start and
its 19th second (the counters' change over the run: diverted,
undiverted, each fallback), and from then runs `conn.bt` for 2 s: for
each connection, its calls run by its rtc work item (`svc_rtc_work`)
against those received by nfsd threads (`svc_tcp_recvfrom` from a task
named nfsd), its work CPU, where its work item last ran, its
`SVC_RTC_THREAD_CALLS` flag (its calls sent to threads) and its
addresses.  `conn.awk` gives a line a connection and which took the
thread path.  `placement.sh 1218 1219`.

## Round 156's result: every connection on rtc; a moved one stays behind

| run | IOPS | a / b M | p99 ms a / b | server busy | server us a READ | client us a READ | rebalanced | placed on 8-55 at s5 / s15 / s25 | undiverted | through threads (2 s) |
|---|---|---|---|---|---|---|---|---|---|---|
| `t2-r1218` | 3.04 M | 1.52 / 1.51 | 9.37 / 9.37 | 72% | 15.16 | 11.84 | 9 | 82 / 91 / 91 | 490 | 0 of 96 |
| `t2-r1219` | 3.31 M | 1.77 / 1.54 | 3.23 / 7.31 | 78% | 15.08 | 11.60 | 3 | 79 / 89 / 96 | 488 | 0 of 96 |

- **The rtc path takes every connection:** all 96 ran through their work
  items, none through threads; 488 to 490 calls undiverted against 56
  to 59 million diverted.  `fallback_partial` is 2 to 3% of the calls.
  The 1.6 billion undiverted since boot were the WRITE runs'.
- **A moved connection's work item stays on its old CPU.**  In
  t2-r1218 the rebalancer moved five connections off CPU 10 to CPUs 36
  and 37 (`conn->cpu`), and `conn.bt` saw all five still run on CPU 10,
  each at 7,906 calls a second against 25,000 to 36,000 for the others:
  six connections on one CPU, CPU 36 counted at four connections and 27%
  busy.  A work item queued while it runs is queued where it runs (the
  workqueue never runs one on two CPUs at once), and a busy connection's
  is kicked again before each pass ends, so `queue_work_on()` to the new
  CPU never takes.  The rebalancer counts the move as made, finds CPU
  10 the busiest still, and moves another: the more moves, the worse
  the run (t2-r1216 2 moves, 3.49 M; t2-r1219 3, 3.31 M; t2-r1217 4,
  3.44 M; t2-r1218 9, 3.04 M).
- **The fix (Mike, 19 November: "yes, fix the work item migration in
  sunrpc, use a Fixes tag in a new commit"):** `cae2f2ea2e5b1`, "SUNRPC:
  Run a moved rtc connection's work item on its new CPU", `Fixes:
  4c084f9754fe1`.  Each connection has a second work item; a pass on the
  wrong CPU does no work, makes the other one active and queues it, not
  running anywhere, on the connection's CPU.  `svc_xprt.o` builds at W=1
  with no warning.  To run it, the server's stack is rebuilt
  (`build-svcstack.sh`: the struct change re-versions sunrpc's exports)
  and swapped in, then the WRITE and READ passes and placement runs.
