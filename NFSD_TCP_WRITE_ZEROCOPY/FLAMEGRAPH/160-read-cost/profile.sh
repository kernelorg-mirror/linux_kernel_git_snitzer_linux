#!/bin/bash
# profile.sh <n> [iodepth]: FLAMEGRAPH round 160, where the server's
# 14.5 us of CPU a 4 KiB READ goes (Mike, 19 November: "now profile where
# the 14.5 us per READ goes").  One settled-iops.sh run of nfs4k.sh
# <n>:<iodepth> (96) on round 159's stack (both rtc fixes), and from the
# load's eighth second 8 s of perf record -a -g at 999 Hz on the server
# (/dev/shm/zc-s160-<n>.data, kept), the READs counted around it.  Each
# sample goes to one stage: walking its stack from the leaf up, the first
# frame that stages.awk names, so a frame is charged where it was called
# from (a memcpy under tcp_sendmsg is the send's).  Samples are time:
# stage samples / 999 / READs in the window is us a READ, for nfsd's
# CPUs (8-55) and the rings' (0-7, 56-63) apart.  Into $L/t2-r<n>/:
# stages.txt (the table), leaf-nfsd.txt and leaf-rings.txt (the leaf
# symbols by us a READ), nfsd.svg and rings.svg (flamegraphs, idle left
# out) with their .folded stacks.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?profile.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-s160-$n.data; P=../FLAMEGRAPH/160-read-cost; FG=${FG:-/opt/FlameGraph}
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
reads() { awk '/^proc3 /{print $9}' /proc/net/rpc/nfsd; }
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	r0=$(reads); t0=$(date +%s.%N)
	perf record -q -N -a -g -e cycles -F 999 -o $D -- sleep 8 > /dev/null 2>&1
	r1=$(reads); t1=$(date +%s.%N)
	echo "reads $((r1 - r0)) seconds $(echo "$t1 - $t0" | bc)" > $O/prof-window.txt
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
R=$(awk '{print $2}' $O/prof-window.txt)
perf script -i $D -F comm,cpu,ip,sym 2>/dev/null | awk -v reads=$R -v hz=999 -v out=$O -f $P/stages.awk > $O/stages.txt
for g in nfsd rings; do
	[ -s $O/$g.folded ] && $FG/flamegraph.pl --title "t2-r$n $g CPUs, 4 KiB READ IOPS" $O/$g.folded > $O/$g.svg 2>/dev/null
done
cat $O/stages.txt
exit $rc
