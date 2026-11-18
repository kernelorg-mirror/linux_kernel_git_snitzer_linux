#!/bin/bash
# prof-srv.sh <n> [iodepth]: FLAMEGRAPH round 133, the server at
# iodepth 96 (Mike, 18 November: "take a fresh profile of the server at
# iodepth 96").  Round 130's prof-both.sh for the server alone: one
# settled-iops.sh run of nfs4k.sh <n>:<iodepth> and, from the load's
# eighth second, 8 s of perf record -a -g at 999 Hz into
# /dev/shm/zc-p133-<n>.data, with -N (no build-id cache): nvme8, the
# root disk, is fragile and nearly full (Mike: no bulk IO there while
# runs go).  Into $L/t2-r<n>/: srv-{self,children,comm,cpu}.txt.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-srv.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-p133-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	perf record -q -N -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
R="perf report -i $D --stdio"
$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/srv-self.txt
$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250 > $O/srv-children.txt
$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/srv-comm.txt
$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/srv-cpu.txt
exit $rc
