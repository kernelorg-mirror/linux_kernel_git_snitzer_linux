# Round 130: the server and the client at 3 M

Mike, 18 November: "take a fresh profile at 3.07 M (ensure the
temporary module retains the switch at Y), also profile the client
because yes it does seem like the client is the limit now".

tardis2 runs round 129's stack (`/var/tmp/svcstack-541-partial`,
sunrpc `99424F2CE6E026FE151DCE7`) with `svc_rtc_partial_wait_ab` at Y,
read before and after the run.  `prof-both.sh 1052`: one
`settled-iops.sh` run, 8 s of `perf record -a -g` at 999 Hz on both
hosts together from the load's eighth second.  Set against t2-r1045
(round 127, 2.91 M, server only).
