#!/bin/bash
# prof-both137.sh <n> [iodepth]: FLAMEGRAPH round 137, round 130's two-host
# profile on the client with the per-CPU choice reverted (Mike, 18
# November: "profile an IOPS pass with iodepth of 96 so we can decide on
# the next round to work on").  One settled-iops.sh run of nfs4k.sh <n>:<iodepth> and, from
# the load's eighth second, 8 s of perf record -a -g at 999 Hz on both
# hosts at once: the server's in /dev/shm/zc-p137-<n>.data, the client's
# in its /dev/shm/zc-cli137-<n>.data, both kept for annotate.  Into
# $L/t2-r<n>/: srv-{self,children,comm,cpu}.txt and
# cli-{self,children,comm,cpu}.txt.  Only reads the client.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-both137.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-p137-$n.data; C=/dev/shm/zc-cli137-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf record -q -a -g -F 999 -o $C -- sleep 8 > /dev/null 2>&1" &
	perf record -q -N -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
	wait
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
R="perf report -i $D --stdio"
$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/srv-self.txt
$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250 > $O/srv-children.txt
$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/srv-comm.txt
$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/srv-cpu.txt
R="perf report -i $C --stdio"
$SSHC "$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli-self.txt
$SSHC "$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250" > $O/cli-children.txt
$SSHC "$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60" > $O/cli-comm.txt
$SSHC "$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep ." > $O/cli-cpu.txt
exit $rc
