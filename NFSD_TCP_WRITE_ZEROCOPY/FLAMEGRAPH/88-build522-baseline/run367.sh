#!/bin/bash
# run367.sh: round 88, the profile on build 522's second boot
# (7.3.3-14.hs.522.canary) to choose the next round's focus.  The tip's
# sunrpc is in updates/ on purpose (srcversion F99F549CC7AF44FA39DAE34,
# $L/sunrpc-522-tip), so this checks that it is the one loaded instead of
# refusing a non-empty updates/.  Four two-port loan runs, each as round
# 76's: t2-r367 and t2-r368 counted (cycles, instructions, L1D and L2D
# refills), t2-r369 cycles with call graphs (frame pointers, as round 77's
# t2-r327), t2-r370 with the held mark for the region's exact peak.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
SRCV=F99F549CC7AF44FA39DAE34
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = $SRCV ] || { say "sunrpc loaded is not the tip's ($SRCV)"; exit 1; }
for run in ${RUNS:-367:C 368:C 369:G 370:H}; do
	n=${run%:*} kind=${run#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; echo N > $MARK; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; echo N > $MARK; exit 1; }
	case $kind in H) mark=Y ;; *) mark=N ;; esac
	echo $mark > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; echo N > $MARK; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	case $kind in
	C) PERF="-e cycles,instructions,l1d_cache_refill,l2d_cache_refill" ;;
	G) PERF="-e cycles --call-graph fp" ;;
	H) PERF= ;;
	esac
	if [ -n "$PERF" ]; then
		( timed_load $O/pmu 5 || exit 1
		  perf record -q -C 8-55 -F 1999 $PERF -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	fi
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=b522-$kind REGION_SAMPLE=1 WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; echo N > $MARK; exit 1; }
	wait
	echo N > $MARK
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	case $kind in
	C) perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz ;;
	G) perf script -i /dev/shm/zc-pmu.data -F period,event,ip,sym 2>/dev/null | xz -T8 -3 > $O/pmu-g.script.xz ;;
	esac
	rm -f /dev/shm/zc-pmu.data
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $kind | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most|held' $O/pmu.log | tail -1) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
