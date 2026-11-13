#!/bin/bash
# run601.sh: round 100, the profile on build 531 (7.3.3-16.hs.531.canary,
# the tree's code built in, nothing in updates/) at the settled two-port
# setting of 12 November (512 KiB WRITEs at window 10, budget 8198 pages),
# after the baseline (run577.sh with CELLS=":10 :10", t2-r599, r600).  As
# round 95's run501.sh, on nfsd's CPUs 8-55: t2-r601 counted (cycles,
# instructions, L1D and L2D refills), t2-r602 cycles with call graphs;
# and t2-r603 cycles with call graphs on the rings' CPUs 0-7 and 56-63.
# The region sampled beside each.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
SP=/sys/module/sunrpc/parameters
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -e $SP/svc_tcp_rx_target_window ] || { say "this sunrpc has no svc_tcp_rx_target_window"; exit 1; }
for run in ${RUNS:-601:C 602:G 603:R}; do
	n=${run%:*} kind=${run#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	CPUS=8-55
	case $kind in
	C) PERF="-e cycles,instructions,l1d_cache_refill,l2d_cache_refill" ;;
	G) PERF="-e cycles --call-graph fp" ;;
	R) PERF="-e cycles --call-graph fp"; CPUS=0-7,56-63 ;;
	esac
	( timed_load $O/pmu 5 || exit 1
	  perf record -q -C $CPUS -F 1999 $PERF -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=b531-$kind REGION_SAMPLE=1 WRITES=64 BUDGET_PAGES=8198 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; exit 1; }
	wait
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	case $kind in
	C) perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz ;;
	G|R) perf script -i /dev/shm/zc-pmu.data -F period,event,ip,sym 2>/dev/null | xz -T8 -3 > $O/pmu-g.script.xz
	     perf report -i /dev/shm/zc-pmu.data --no-children --no-inline -g none --percent-limit 0.3 --sort symbol 2> /dev/null | grep -v '^$' | head -90 > $O/perf-sym.txt ;;
	esac
	rm -f /dev/shm/zc-pmu.data
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $kind | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	echo "    $(grep '^loans' $O/pmu/window.txt)"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
