#!/bin/bash
# spread.sh <n> [iodepth]: FLAMEGRAPH round 147, what fills the server's
# CPUs and what leaves some of them idle (Mike, 19 November: "yes, queue
# the server perf measurement after round 146").  One settled-iops.sh run
# of nfs4k.sh <n>:<iodepth> (96), the client on its updates/ stack, and
# from the load's eighth second 8 s of perf record -a at 999 Hz on the
# server, no call graphs (/dev/shm/zc-s147-<n>.data, kept).  Into
# $L/t2-r<n>/: srv-cpu-comm.txt (samples by CPU and command),
# srv-cpu-sym.txt (by CPU and symbol, the top of each CPU) and
# spread.txt (each CPU's busy, its role, and its samples by kind: rtc
# work items, nfsd threads, softirq, other).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?spread.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-s147-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	perf record -q -N -a -F 999 -o $D -- sleep 8 > /dev/null 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
perf report -i $D --stdio --no-children --sort cpu,comm 2>/dev/null | grep -v '^#' | grep . > $O/srv-cpu-comm.txt
perf script -i $D -F cpu,comm,sym 2>/dev/null | awk -f ../FLAMEGRAPH/147-server-spread/spread.awk -v M=$O/srv-mpstat-w.txt -v S=$O/srv-cpu-sym.txt > $O/spread.txt
sort -k1,1n -k2,2nr $O/srv-cpu-sym.txt -o $O/srv-cpu-sym.txt
exit $rc
