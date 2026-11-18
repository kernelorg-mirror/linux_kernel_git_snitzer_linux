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

## Result (18 November, 17:05 to 17:20): pfifo_fast saves nothing

| Run | Server qdisc | M IOPS (ports) | p99 a; b (ms) | Server busy, us a READ | Client busy (busiest) | Client us a READ |
|---|---|---|---|---|---|---|
| t2-r1072 | fq_codel | 3.57 (1.73 / 1.84) | 3.72; 3.59 | 87%, 15.60 | 84% (91%) | 11.29 |
| t2-r1073 | pfifo_fast | 3.48 (1.67 / 1.81) | 3.65; 3.82 | 85%, 15.63 | 82% (88%) | 11.31 |
| t2-r1074 | fq_codel | 3.53 (1.73 / 1.81) | 4.08; 3.56 | 87%, 15.77 | 84% (89%) | 11.42 |
| t2-r1075 | pfifo_fast | 3.52 (1.69 / 1.83) | 3.69; 3.62 | 87%, 15.82 | 84% (90%) | 11.45 |

fq_codel: 3.55 M at 15.685 us of the server's CPU a READ; pfifo_fast:
3.50 M at 15.725.  Both inside the runs' spread: the qdisc's root lock
that round 133's profile showed (0.22 us) does not come back as a
saving, as round 07 found for WRITEs.  `settled.sh` keeps `fq_codel`;
both ports were on it again after each run.  No new kernel warning.
The client is back on build 541's stack.  No pass: nothing is kept.

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

Done at 17:24: built from `6735e8c25f3a8` (8 modules, no warning but
the compiler notes; sunrpc `C4A23C35035DD3A1E1C967B`, the client's P
build's source, without the partial wait's switch; nfsd
`78F201D2B9A190D3A538FA9`), swapped with no client mounted, the host at
the settled setting, kdump armed.  `svcstack-541-partial` stays in
/var/tmp for a swap back.
