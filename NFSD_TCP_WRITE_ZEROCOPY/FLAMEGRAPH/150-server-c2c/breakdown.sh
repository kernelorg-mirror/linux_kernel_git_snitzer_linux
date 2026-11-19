#!/bin/bash
# breakdown.sh <n> [iodepth]: FLAMEGRAPH round 150, the server's CPU a
# READ by stage, and where it misses (Mike, 19 November: "then the server
# breakdown using c2c on the server"; tardis2 has no SPE, so no perf c2c:
# the core PMU's miss events instead).  One settled-iops.sh run of
# nfs4k.sh <n>:<iodepth> (96), the client on its updates/ stack, and from
# the load's eighth second 8 s of perf record -a -g on the server with
# three sampled events: cycles at 999 Hz (the CPU by stage),
# l2d_cache_refill_rd every 20000 (reads that missed the core's L2: from
# another core, the L3 or memory) and ll_cache_miss_rd every 5000 (reads
# that went to memory), one perf record each, at once;
# /dev/shm/zc-s150-<n>-<event>.data, kept.  Into
# $L/t2-r<n>/: srv-<event>-self.txt and srv-<event>-children.txt for each.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?breakdown.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-s150-$n
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	perf record -q -N -a -g -e cycles -F 999 -o $D-cycles.data -- sleep 8 > /dev/null 2>&1 &
	perf record -q -N -a -g -e l2d_cache_refill_rd -c 20000 -o $D-l2d_cache_refill_rd.data -- sleep 8 > /dev/null 2>&1 &
	perf record -q -N -a -g -e ll_cache_miss_rd -c 5000 -o $D-ll_cache_miss_rd.data -- sleep 8 > /dev/null 2>&1 &
	wait
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
for e in cycles l2d_cache_refill_rd ll_cache_miss_rd; do
	R="perf report -i $D-$e.data --stdio -g none"
	$R --no-children --sort sym --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -150 > $O/srv-$e-self.txt
	$R --children --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/srv-$e-children.txt
done
for e in cycles l2d_cache_refill_rd ll_cache_miss_rd; do echo "$e $(perf report -i $D-$e.data --stdio 2>/dev/null | grep -m1 '^# Samples')"; done > $O/srv-events.txt
exit $rc
