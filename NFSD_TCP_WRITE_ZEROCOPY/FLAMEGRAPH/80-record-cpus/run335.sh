#!/bin/bash
# run335.sh: round 80, which CPUs a loaned record's pieces and release run
# on.  One two-port loan run on the canary's own stack, as round 74's, with
# record-cpus.bt for five seconds in the window in place of perf (t2-r335).
# The trace costs the run its rate: the figure is not one to compare.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
BT=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/80-record-cpus/record-cpus.bt
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -z "$(ls /lib/modules/$(uname -r)/updates 2>/dev/null)" ] || { say "updates/ is not empty"; exit 1; }
O=$L/t2-r335; mkdir -p $O
client_free || { say "client not free"; exit 1; }
echo N > /sys/module/page_pool/parameters/rx_region_held_mark
./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; exit 1; }
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
dmesg > $O/dmesg.0
( timed_load $O/pmu 5 || exit 1
  bpftrace $BT > $O/record-cpus.txt 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=bt WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
	{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; exit 1; }
wait
dmesg > $O/dmesg.1
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
say "t2-r335 traced | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | new kernel warnings $hung"
