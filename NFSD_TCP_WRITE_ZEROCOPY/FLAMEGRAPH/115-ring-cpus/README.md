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
