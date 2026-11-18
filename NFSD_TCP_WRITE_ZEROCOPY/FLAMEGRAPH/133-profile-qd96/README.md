# Round 133: the server at iodepth 96

Mike, 18 November: "take a fresh profile of the server at iodepth 96".
Round 132 left the server the lower ceiling: at iodepth 96 (t2-r1063,
3.50 M) the server took 15.54 us of CPU a READ on 64 CPUs (4.12 M at
100%), the client 11.38 on 48 (4.22 M).

The client runs round 132's P stack (the tree at `e476bfc6bb64e`, its
calls counted per CPU), as in t2-r1063; the server round 129's stack at
the settled setting.  `prof-srv.sh 1065` (iodepth 96): one
settled-iops.sh run with 8 s of perf on the server, its data in
/dev/shm and no build-id cache, so that nothing but the reports goes to
the root disk.  The client goes back to build 541's stack after.
