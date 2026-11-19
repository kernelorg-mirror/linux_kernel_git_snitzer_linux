# Round 159: both rtc placement fixes, the passes and IOPS

Mike, 19 November: "stop 158, just redo as 159".  Round 158 stopped
after t2-r1222 (3.27 M, port a 1.47 M) and its run in flight, t2-r1223:
the move fix alone left about six connections never placed.  A
connection is placed only on a receive kick in softirq, and a busy one's
segments arrive while its work item holds the socket and are processed
at its release, in process context: never placed, it runs where its
work item last ran, out of the balance's sight (t2-r1218: five, on CPUs
10 and 30).  `efe70d0679e89`, "SUNRPC: Place an rtc connection on a
receive outside softirq too" (`Fixes: 6db0c38527abe`), places it there.

`run159.sh`: the stack from the tip with both fixes (`cae2f2ea2e5b1`
and `efe70d0679e89`) built and swapped in, then the WRITE pass
(t2-r1224) and the READ pass (t2-r1225), four IOPS runs with no tracing
(t2-r1226 to r1229) and one with `conn.bt` (t2-r1230).

## Results (19 November, 22:01 to 22:30)

Stack: sunrpc `FF8835E8473FBC55068D0B9` (both fixes), nfsd
`78F201D2B9A190D3A538FA9`; the settled setting before and after.

| run | pass | GB/s | mark | |
|---|---|---|---|---|
| `t2-r1224` | WRITE | 77.90 | 77.6 | PASS, no drive stepped down, no new kernel warning |
| `t2-r1225` | READ | 82.17 | 77.6 | PASS, no new kernel warning |

| run | stack | IOPS | a / b M | p99 ms a / b | server us a READ | moves | placed at s5 | CPUs with 1 / 2 / 3 / 4+ |
|---|---|---|---|---|---|---|---|---|
| `t2-r1218` (156) | neither fix | 3.04 M | 1.52 / 1.51 | 9.37 / 9.37 | 15.16 | 9 | 82 | |
| `t2-r1222` (158) | move fix | 3.27 M | 1.47 / 1.80 | 12.52 / 3.16 | 14.87 | 5 | 75 | 10 / 38 / 0 / 0 |
| `t2-r1223` (158) | move fix | 3.52 M | 1.79 / 1.73 | 3.00 / 3.13 | | | | |
| `t2-r1226` | both | 3.54 M | 1.76 / 1.78 | 3.06 / 3.13 | 14.46 | 6 | 96 | 2 / 44 / 2 / 0 |
| `t2-r1227` | both | 3.57 M | 1.81 / 1.76 | 3.16 / 3.29 | 14.70 | 6 | 96 | 2 / 45 / 0 / 1 |
| `t2-r1228` | both | 3.51 M | 1.76 / 1.75 | 3.19 / 3.26 | 14.77 | 4 | 96 | 1 / 46 / 1 / 0 |
| `t2-r1229` | both | 3.49 M | 1.73 / 1.76 | 3.23 / 3.23 | 14.85 | 2 | 96 | 0 / 48 / 0 / 0 |
| `t2-r1230` (conn.bt) | both | 3.49 M | 1.74 / 1.75 | 3.06 / 3.13 | 15.04 | 2 | 96 | 1 / 46 / 1 / 0 |

- **Every connection is placed by the fifth second** (75 to 82 before),
  and `conn.bt` in t2-r1230 finds none never placed and none running
  off its work CPU (94 of 96 called in its 2 s, all on the rtc path).
- **The runs hold together:** 3.49 to 3.57 M (mean 3.53 M) over four
  untraced runs, against 3.04 to 3.64 M before; the ports within 0.05
  M of each other, p99 3.1 to 3.3 ms on both, every ring 386K to 443K
  frames a second.
- **Placement no longer limits:** t2-r1229 has exactly two connections
  on every nfsd CPU, 98% busy, and is the lowest of the four.  The
  server is CPU-bound at 14.5 to 14.9 us a READ; more IOPS is less CPU
  a READ.  What remains of placement: an initial three (once four) on
  one CPU beside one on another, which the rebalancer leaves.
- **Kept:** `cae2f2ea2e5b1` and `efe70d0679e89`, to be folded into
  `4c084f9754fe1` and `6db0c38527abe` at the next regroup.  tardis2's
  `updates/` holds this stack; the stacks before are in
  `/dev/shm/zc-svcstack-move/stack` and `/dev/shm/zc-stack-pre-move/`.
- `placement.sh`'s "off their work CPU" line counted `conn.awk`'s own
  summary line in t2-r1230 (a false 1); it reads the connection lines
  only now.
