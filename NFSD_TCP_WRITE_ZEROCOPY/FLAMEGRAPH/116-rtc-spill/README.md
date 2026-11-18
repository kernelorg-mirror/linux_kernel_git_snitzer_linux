# Round 116: the rtc balance spilling onto the ring CPUs

Mike, 18 November: "implement the spill weight option and A/B it".

## The change

`219a15abdd219` "SUNRPC: Let the rtc balance spill onto other CPUs as a
last resort": `svc_rtc_spill_cpus`, CPUs the balance may place a work
item on besides `svc_rtc_cpus`, each counting as already holding
`svc_rtc_spill_weight` work items (2): every work CPU fills to that many
before a spill CPU takes its first.  A receive on a spill CPU does not
take it out of the placement.  Only with `svc_rtc_balance` and a
non-empty `svc_rtc_cpus`; empty, the default, changes nothing.
`settled.sh` holds it empty at rest (`EXP_RTC_SPILL_CPUS`,
`EXP_RTC_SPILL_WEIGHT` for a run).  Built into `/var/tmp/svcstack-541-spill`
(sunrpc `B0F853C996AAA852046407E`, nfsd `DFC417DB5E771ABF21E2D4D`), W=1, no
warning.  No KUnit case: the rtc suite, where placement is tested, is not
to be run on tardis2 (AGENTS.md).

With 96 connections and 48 work CPUs at weight 2, every work CPU takes
two and no ring CPU takes one: the A/B shows whether that holds; at weight
1 every ring CPU would take one before a work CPU takes its second.

## The plan

    cd xeu-rig
    STACK=/var/tmp/svcstack-541-spill/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-spill ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 993; ./read-check.sh 994
    ../FLAMEGRAPH/116-rtc-spill/ab-spill.sh 995:w1 996:off 997:w1 998:off 999:w2 1000:off

Weight 1 is the case that puts work on the rings (one connection each,
16 of 96); weight 2 should place none there with 96 connections, so one
run of it checks the placement.

## Results (18 November, 05:12 to 05:42)

Passes on the spill stack, spill off: WRITE t2-r993 **75.20 GB/s**
(PASS at 74.6, no drive stepped down), READ t2-r994 **79.44 GB/s** (PASS
at 71.5; 76% of the drives' 104.58 with local fio, 81% of iperf3's 98.31
on the wire).  No new kernel warning in any run.

| Run | Spill | M IOPS (ports) | p99 a; b (ms) | Work CPUs (max) | Ring CPUs (max) | Server busy | On rings |
|---|---|---|---|---|---|---|---|
| t2-r995 | weight 1 | **2.78** (1.38 / 1.40) | 3.26; 3.26 | 81.3% (94%) | 80.3% (95%) | 80% | 15 of 96 |
| t2-r996 | off | 2.58 (1.30 / 1.28) | 4.49; 4.75 | 89.0% (95%) | 22.7% (53%) | 73% | 0 |
| t2-r997 | weight 1 | **2.73** (1.30 / 1.44) | 3.46; 3.19 | 82.2% (95%) | 77.1% (94%) | 81% | 14 of 96 |
| t2-r998 | off | 2.57 (1.29 / 1.28) | 4.62; 4.69 | 89.4% (96%) | 21.7% (54%) | 73% | 0 |
| t2-r999 | weight 2 | 2.59 (1.31 / 1.28) | 4.88; 5.01 | 89.9% (97%) | 22.0% (44%) | 73% | 0 |
| t2-r1000 | off | 2.58 (1.28 / 1.30) | 5.93; 4.62 | 88.5% (95%) | 26.2% (91%) | 73% | 0 |

Weight 1: 2.755 M on the mean against 2.577 off (three runs), **+6.9%**,
and p99 about 30% lower; its lower run beats every run off by 0.15 M.
The ring CPUs take one connection each at most (14 and 15 of 16), and no
CPU reaches 100%, where round 115's 0-63 put one or two on each and
saturated them.  t2-r995 ran on files just written (after the passes),
t2-r997 on kept ones, at the same rate.  Weight 2 places nothing on the
rings at 96 connections (48 work CPUs at two each fill all 96), so it
runs as off: it matters only with more connections than twice the work
CPUs.  t2-r1000 was not needed (Mike: "we already got it"); it had
started and was left to finish.

Not settled: `settled.sh` holds the spill empty until Mike says.
Holding it for IOPS would be `EXP_RTC_SPILL_CPUS=0-7,56-63
EXP_RTC_SPILL_WEIGHT=1` in `settled-iops.sh`; for WRITEs and the
passes it stays off.
