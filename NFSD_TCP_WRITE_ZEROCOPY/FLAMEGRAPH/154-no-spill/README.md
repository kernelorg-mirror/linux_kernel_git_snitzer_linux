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
