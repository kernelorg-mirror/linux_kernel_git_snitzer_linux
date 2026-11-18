# Round 127: a profile at the new settled-iops.sh

Mike, 18 November: "take a fresh profile with the new settled-iops.sh".
`settled-iops.sh` now holds the rebalancer (`svc_rtc_rebalance_ms` 250)
and `tcp_limit_output_bytes` 1 MiB on top of round 124's stack
(`/var/tmp/svcstack-541-rebal`, sunrpc `32BA58F914BEB5F3CF491F8`).  One
`prof-srv.sh` run, t2-r1045, against t2-r1029 (round 124, 2.88 M).
