# Round 137: a profile of 4 KiB READs with the per-CPU choice reverted

Mike, 18 November: "profile an IOPS pass with iodepth of 96 so we can
decide on the next round to work on".  The client on its `updates/`
stack (the tip with round 136's reverts, sunrpc
`2355B960B6A9951899AF6B4`), the server on the tip's svc stack, both at
the settled setting.  `prof-both137.sh 1128`: one `settled-iops.sh` run
of `nfs4k.sh 1128:96` with 8 s of `perf record -a -g` at 999 Hz on both
hosts from the load's eighth second (the server's with `-N` and in
`/dev/shm`: nvme8 is fragile).

Before it, at iodepth 96: 3.51 to 3.63 M IOPS (t2-r1120, r1124, r1125);
at 128: 3.65 and 3.56 M (t2-r1126, r1127), both hosts' busiest CPUs at
93 to 100%.
