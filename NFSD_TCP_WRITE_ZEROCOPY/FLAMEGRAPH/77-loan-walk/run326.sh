#!/bin/bash
# run326.sh: round 77's two runs before any code, on the canary's own
# stack.  t2-r326: the held mark on (page_pool.rx_region_held_mark=Y, set
# at rest), for the region's exact peak, with the 20 ms sample beside it
# and round 74's counted profile.  t2-r327: the mark off, cycles only with
# call graphs (frame pointers), for who calls the loan's per-segment
# bookkeeping and the spinlocks.  Otherwise each run as round 76's.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -z "$(ls /lib/modules/$(uname -r)/updates 2>/dev/null)" ] || { say "updates/ is not empty"; exit 1; }
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
for run in 326:Y 327:N; do
	n=${run%:*} mark=${run#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; echo N > $MARK; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; echo N > $MARK; exit 1; }
	echo $mark > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; echo N > $MARK; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	if [ $mark = Y ]; then
		PERF="-e cycles,instructions,l1d_cache_refill,l2d_cache_refill"
	else
		PERF="-e cycles --call-graph fp"
	fi
	( timed_load $O/pmu 5 || exit 1
	  perf record -q -C 8-55 -F 1999 $PERF -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu-$mark REGION_SAMPLE=1 WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; echo N > $MARK; exit 1; }
	wait
	echo N > $MARK
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	if [ $mark = Y ]; then
		perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
	else
		perf script -i /dev/shm/zc-pmu.data -F period,event,ip,sym 2>/dev/null | xz -T8 -3 > $O/pmu-g.script.xz
	fi
	rm -f /dev/shm/zc-pmu.data
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n mark $mark | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/pmu.log | tail -1) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
