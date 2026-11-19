# Round 160: where the server's 14.5 us of CPU a 4 KiB READ goes

Mike, 19 November: "now profile where the 14.5 us per READ goes".  Round
159 left the server CPU-bound at 14.46 to 14.85 us a READ (mpstat busy
times CPUs over IOPS), every nfsd CPU 98% busy with two connections
each.  `profile.sh 1231`: one settled-iops.sh run of nfs4k.sh at
iodepth 96 on round 159's stack, 8 s of `perf record -a -g` at 999 Hz
from the load's eighth second, the READs counted around it.
`stages.awk` charges each sample to one stage, the first frame from the
leaf up that it names (a memcpy under `tcp_sendmsg` is the send's, a
softirq taken inside `tcp_sendmsg` is the softirq's), leaves idle out,
and gives us a READ for nfsd's CPUs (8-55) and the rings' (0-7, 56-63).
Round 150 (t2-r1199, on the stack before the spill's removal and both
rtc fixes) is the comparison; its profile can no longer be read for
sunrpc and nfsd symbols, the modules having been swapped since.
