#!/bin/bash
# run234.sh: round 62, the NVMe SGL map walk on build 514 at the client's new
# defaults.  Two loan runs on both ports, each after a trim of the eight and
# 120 s of rest, as round 55's run168 and run170:
#   t2-r234  sglwalk-kp.bt for 10 s inside the window (pieces a WRITE and a
#            request, requests and splits a WRITE)
#   t2-r235  cycles, instructions and L1 and L2 refills sampled on nfsd's CPUs
#            (8-55) for 8 s, read by round 55's pmu.py
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
R=../FLAMEGRAPH/62-map-walk
say() { echo "$(date +%T) $*"; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
echo N > /sys/module/page_pool/parameters/rx_region_held_mark
prep() {
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $1/apply-$2.txt 2>&1 || { say "$2 not settled: $(grep -E 'BAD|FAILED' $1/apply-$2.txt | head -3)"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
}
O=$L/t2-r234; mkdir -p $O; prep $O trace
( timed_load $O/trace 5 || exit 1
  timeout -s INT 10 bpftrace $R/sglwalk-kp.bt > $O/sglwalk.txt 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=trace WRITES=128 ./twoport.sh $O/trace ab > $O/trace.log 2>&1 ||
	{ say "trace failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/trace.log | head -3)"; exit 1; }
wait
say "t2-r234 trace | $(grep '^total' $O/trace.log) | $(grep -c . $O/sglwalk.txt) lines"
O=$L/t2-r235; mkdir -p $O; prep $O pmu
( timed_load $O/pmu 5 || exit 1
  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o $O/pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
	{ say "pmu failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; exit 1; }
wait
say "t2-r235 pmu | $(grep '^total' $O/pmu.log) | $(ls -s $O/pmu.data)"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
perf script -i $O/pmu.data -F ip,sym,period,event 2>/dev/null > $O/pmu.script
../FLAMEGRAPH/55-nvme-sgl-walk/pmu.py $O/pmu.script > $O/pmu.txt; cat $O/pmu.txt
