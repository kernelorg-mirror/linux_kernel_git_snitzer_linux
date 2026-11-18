# Round 135: the server ports' qdisc for 4 KiB READs

Mike, 18 November: "run the qdisc A/B next, quick".  Round 133's
profile put 0.22 us of the server's CPU a READ in the qdisc's root lock
(`_raw_spin_lock` in `__dev_xmit_skb`, `__qdisc_run`,
`neigh_hh_output`): each port has 4 transmit queues, each under its own
`fq_codel`, so the 64 CPUs' replies pass 8 root locks.  `pfifo_fast`
is lockless (`TCQ_F_NOLOCK`) and bypassed while empty.  Round 07 found
it saved nothing for WRITEs, whose sends are few and large; a 4 KiB
READ's reply is one small send each.  `noqueue` is not tried: round 31
found it drops sends when a queue stops.

`ab-qdisc.sh 1072:fq_codel 1073:pfifo_fast 1074:fq_codel
1075:pfifo_fast`: the client on round 132's P stack, iodepth 96,
`SRV_QDISC=` for the B runs (twoport.sh sets it on both ports and puts
`fq_codel` back after).  After the passes of round 134; the client back
on build 541's stack after.

## After: the server on the tip's own stack

Mike: "yes, build the tip's svc stack after round 135".  The server has
run round 129's A/B stack (`/var/tmp/svcstack-541-partial`, its switch
held at Y) since that round; the tip differs from it only by the switch
and round 132's client-call counts in sunrpc.  After the A/B, with no
run going (the build writes about 400 MB to the root disk):
`build-svcstack.sh /var/tmp/svcstack-541-tip` from the tip, then
`STACK=/var/tmp/svcstack-541-tip/stack SUNRPC_KO=none
./step.sh ... swap-sunrpc.sh`, then `settled.sh apply`, the sunrpc
srcversion and the switch's absence checked.
