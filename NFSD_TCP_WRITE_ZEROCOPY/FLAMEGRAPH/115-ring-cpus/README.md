# Round 115: nfsd's work on the ring CPUs too, for IOPS

Mike, 18 November: "run the ring CPUs A/B with fh_borrow_slots 48".

## Why

Round 113 (t2-r972) found the server the limit at 2.54 M 4 KiB READs a
second: the 48 work CPUs (8 to 55) 89.4% busy on the mean, the 16 ring
CPUs (0 to 7, 56 to 63) 23.6%.  The rtc work items, about 94% of a work
CPU's samples, may run only on `svc_rtc_cpus`, 8 to 55 on tardis2 since
15 November (round 104's t2-r675 found work items on transmit CPUs at 97
to 100%, which is why they were confined).  About 12 CPUs of the rings'
time are idle; this round lets the work items onto them.

## The run

The stack of round 114 (sunrpc `85537945BAC1207EDA22E45`, nfsd
`DFC417DB5E771ABF21E2D4D`), `settled-iops.sh` with all three borrows
held (`fh_borrow_slots` 48 since `45a890a5bf8d2`: these are its first
runs at 48), the client at scope `cpu`.  Three runs a side in turns,
`svc_rtc_cpus` 0-63 against the settled 8-55:

    ../FLAMEGRAPH/115-ring-cpus/ab-rtc.sh 987:0-63 988:settled 989:0-63 990:settled 991:0-63 992:settled

`settled.sh apply` after each run puts 8-55 back.  Only the work items
move: nfsd's threads (2% of samples), RPS/RFS and the rings stay as
settled.  It is for IOPS: the settled 8-55 stays for WRITEs.

## Results (18 November, 04:51 to 05:11)

| Run | svc_rtc_cpus | M IOPS (ports) | p99 a; b (ms) | Work CPUs (max) | Ring CPUs (max) | Server busy |
|---|---|---|---|---|---|---|
| t2-r987 | 0-63 | 2.61 (1.27 / 1.34) | 4.95; 4.55 | 72.4% (95%) | 88.1% (100%) | 75% |
| t2-r988 | 8-55 | 2.53 (1.26 / 1.27) | 5.34; 4.36 | 86.7% (98%) | 24.5% (85%) | 71% |
| t2-r989 | 0-63 | 2.53 (1.35 / 1.18) | 4.05; 5.41 | 71.8% (95%) | 88.0% (100%) | 74% |
| t2-r990 | 8-55 | 2.52 (1.25 / 1.28) | 5.01; 4.75 | 89.9% (96%) | 20.7% (32%) | 71% |
| t2-r991 | 0-63 | 2.54 (1.23 / 1.31) | 5.21; 4.95 | 73.2% (97%) | 90.7% (100%) | 76% |
| t2-r992 | 8-55 | 2.45 (1.22 / 1.23) | 6.19; 6.19 | 89.3% (94%) | 24.8% (48%) | 73% |

0-63 2.560 M on the mean, 8-55 2.500 (t2-r992's 2.45 the lowest of the
night at this shape).  No new kernel warning.  `fh_borrow_slots` 48 in
every run: 99.8% of handles hit, no evictions.

At 0-63 the ring CPUs fill to 88 to 91% with their own softirq and work
items, the busiest at 100%, while the work CPUs drop to 72 to 73%, and
one port runs slower than the other, which one changing from run to run
(1.18 to 1.35 M a port).  The balance counts only work items placed
(`svc_rtc_place()`, `svc_rtc_pick_least()`): 96 connections over 64
CPUs put one or two on each ring CPU as on an idle one, though it
already spends 21 to 25% on its port's softirq; the port whose rings take
two saturates.  Hence round 116: the ring CPUs as a last resort.
