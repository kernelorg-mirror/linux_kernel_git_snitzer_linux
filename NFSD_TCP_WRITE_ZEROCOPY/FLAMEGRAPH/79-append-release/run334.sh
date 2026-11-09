#!/bin/bash
# run334.sh: round 79, one counted run on the canary's own stack that keeps
# its perf.data (t2-r334), for perf annotate of __rx_loan_table_append_ref()
# and rx_loan_table_release() against the debug vmlinux.  Otherwise as
# round 74's runs, the held mark off.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -z "$(ls /lib/modules/$(uname -r)/updates 2>/dev/null)" ] || { say "updates/ is not empty"; exit 1; }
[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; exit 1; }
O=$L/t2-r334; mkdir -p $O
client_free || { say "client not free"; exit 1; }
echo N > /sys/module/page_pool/parameters/rx_region_held_mark
./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; exit 1; }
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
( timed_load $O/pmu 5 || exit 1
  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu-C WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
	{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; exit 1; }
wait
cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
mv /dev/shm/zc-pmu.data $O/pmu.data
cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
say "t2-r334 C | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | new kernel warnings $hung"
