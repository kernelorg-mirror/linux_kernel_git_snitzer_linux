#!/bin/bash
# prof-srv.sh <n>: FLAMEGRAPH round 113, the server's per-READ cost at
# 2.54 M IOPS with both borrows (Mike, 18 November: "profile the server
# at 2.54 M with both borrows").  One settled-iops.sh run of nfs4k.sh <n>
# (iodepth 64, nconnect 12, both borrows held, the client at scope cpu)
# and, from the load's second 8, 8 s of perf record -a -g at 999 Hz on
# the server, kept in /dev/shm/zc-p113-<n>.data for annotate.  Into
# $L/t2-r<n>/: srv-{self,children,comm,cpu}.txt, srv-busiest.txt (what
# runs on the four busiest CPUs, by command and by symbol).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-srv.sh <n>}
O=$L/t2-r$n; D=/dev/shm/zc-p113-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	perf record -q -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n; rc=$?
wait
R="perf report -i $D --stdio"
$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/srv-self.txt
$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250 > $O/srv-children.txt
$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/srv-comm.txt
$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/srv-cpu.txt
: > $O/srv-busiest.txt
for c in $(sort -k1 -rn $O/srv-cpu.txt | awk '{print $2}' | head -4); do
	{ echo "== CPU $c"; $R --no-children -g none --cpu $c --sort comm --percent-limit 1 2>/dev/null | grep -v '^#' | grep . | head -12
	  $R --no-children -g none --cpu $c --sort sym --percent-limit 1 2>/dev/null | grep -v '^#' | grep . | head -25; } >> $O/srv-busiest.txt
done
exit $rc
