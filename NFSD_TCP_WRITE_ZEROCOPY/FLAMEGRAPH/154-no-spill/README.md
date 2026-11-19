# Round 154: no spill onto the ring CPUs

Mike, 19 November: "should we try _not_ spilling to the rings CPUs?  We
did that to give more CPU headroom for NFSD work, but if it is causing
thrashing on the rings CPUs is it getting in the way?", then "yes, queue
round 154 after t2-r1210".

Round 153's settled B (t2-r1209, Release 17, 4 rings) found the spill
uneven: twelve of the sixteen ring CPUs (0-7, 56-63) carried 38 to 63%
nfsd work on their softirq and ran 78 to 100% busy, four carried almost
none (0, 3, 61, 63 at 30 to 44%); nfsd's own CPUs (8-55) 91%.  A ring
whose CPU is full waits behind nfsd threads, and the two share the
core's L1 and L2.

`ab-spill.sh`, settled Release 17, no reload: S (settled-iops.sh's spill
to 0-7,56-63 at weight 1) and N (`EXP_RTC_SPILL_CPUS=none`), S N S N as
t2-r1211 to r1214, each round 151's `packets.sh`, with the ring CPUs'
busy and nfsd share from its mpstat.  The spill is held only by
`settled-iops.sh`, so the WRITE and READ passes are not changed.

## Result: no spill takes 1.05 us less of the server's CPU a READ

`ab-spill.sh`, 19:50 to 20:03; no new kernel warning, the host at the
settled setting after.

| run | arm | IOPS | a / b M | p99 ms a / b | server busy | server us a READ | client us a READ | ring rx/s | ring CPUs busy (nfsd on them) | nfsd CPUs busy |
|---|---|---|---|---|---|---|---|---|---|---|
| `t2-r1211` | S | 3.52 M | 1.80 / 1.72 | 3.36 / 3.59 | 89% | 16.18 | 11.45 | 395 to 428K | 88% (45%) | 90% |
| `t2-r1212` | N | 3.64 M | 1.82 / 1.82 | 3.00 / 3.06 | 82% | 14.42 | 11.21 | 418 to 427K | 30% (3%) | 98% |
| `t2-r1213` | S | 3.47 M | 1.65 / 1.83 | 5.34 / 4.75 | 83% | 15.31 | 11.20 | 356 to 447K | 77% (36%) | 86% |
| `t2-r1214` | N | 3.29 M | 1.63 / 1.66 | 6.59 / 4.88 | 77% | 14.98 | 11.53 | 364 to 402K | 29% (4%) | 94% |

Means: **S 15.75 us a READ at 3.50 M, N 14.70 at 3.47 M**: 1.05 us (6.7%)
less of the server's CPU a READ without the spill, at the same IOPS; the
client's the same (11.33 against 11.37).  Both N runs are below both S
runs, and below every run of round 153 (15.50 to 15.87).

- **What the spill did:** S put 36 to 45% nfsd work on the ring CPUs,
  which then ran 77 to 88% busy (up to 100% on some); without it they
  run 29 to 30% and nfsd's 48 CPUs 94 to 98%.  The same IOPS come from
  fewer cycles: nfsd's work on a ring CPU, beside the softirq and
  sharing its L1 and L2, costs more than on its own CPUs.
- **The RFS hop rises** from 0.46 to 0.47 a READ to 0.55 to 0.57: with
  no nfsd thread on the ring CPUs, every connection's frames go to
  nfsd's CPUs.
- **The low port is not the spill alone:** t2-r1214 (N) ran both ports
  low (1.63 / 1.66 M).  In each run where a port fell behind, that
  port's files were read unevenly in the window: t2-r1203 nvme2 and
  nvme3 15.8 and 16.1 GB against nvme0's 28.9; t2-r1208 nvme2 17.2;
  t2-r1213 nvme0 18.9; t2-r1214 nvme0 16.0 and nvme7 19.1, against 22
  to 25 GB on the rest.  At 1.3 to 2.0 GB/s of 4 KiB reads a drive is
  far below what local fio gets from it (round 108: 18.78 M IOPS on the
  eight), so this follows the connections whose jobs read those files,
  not the drives.  Where in the server those connections fall behind
  is open.
- **With N, 16 CPUs are 70% idle and nfsd's are the limit.**  The next
  step is to give nfsd some of that back without the cost: a spill to
  the ring CPUs that carry least softirq (the TX-interrupt CPUs, 1, 3,
  5, 7, 57, 59, 61, 63), or a lower spill weight.
