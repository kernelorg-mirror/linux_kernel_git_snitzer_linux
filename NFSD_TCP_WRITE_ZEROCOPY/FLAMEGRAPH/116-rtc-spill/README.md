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
