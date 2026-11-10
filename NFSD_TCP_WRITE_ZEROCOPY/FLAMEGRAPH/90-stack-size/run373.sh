#!/bin/bash
# run373.sh: round 90, page_pool.rx_region_stack 64 against 96 and 128.
# Two-port loan runs as round 88's, the stack set by settled.sh's
# EXP_RX_REGION_STACK (the ports opened again):
#   t2-r373..r378  counted profile, mark off, stacks 64 96 128 128 96 64;
#   t2-r379..r381  the held mark on, no profile, stacks 64 96 128.
# RUNS= and N0= run another list from another run number: round 90's
# second part, RUNS="64:H 96:H 96:H 64:H 64:H 96:H" N0=382.
# Each run keeps both ports' ethtool -S before and after, for the pools'
# recycle_ring (pages to the stack), recycle_ring_full (bulk returns that
# overflowed) and alloc_slow (region allocs).  At the end, 64 again.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
SRCV=F99F549CC7AF44FA39DAE34
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
back() { unset EXP_RX_REGION_STACK; echo N > $MARK
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && ./settled.sh apply > $L/t2-r373-end-apply.txt 2>&1
	say "end: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $L/t2-r373-end-apply.txt)"; }
die() { say "STOPPED: $*"; back; exit 1; }
stats() { for d in enp161s0f0 enp164s0f0; do ethtool -S $d | awk -v d=$d '/rx_pp_(recycle_ring|recycle_ring_full|alloc_slow|alloc_refill|alloc_empty):/ {k=$1; sub(/^ring_[0-9]+_rx_pp_/, "", k); sub(/:$/, "", k); s[k] += $2} END {for (k in s) print d, k, s[k]}'; done; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump not armed"
[ "$(cat /sys/module/sunrpc/srcversion)" = $SRCV ] || die "sunrpc loaded is not the tip's ($SRCV)"
n=${N0:-373}
for run in ${RUNS:-64:C 96:C 128:C 128:C 96:C 64:C 64:H 96:H 128:H}; do
	stk=${run%:*} kind=${run#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || die "root has under 2 GB free"
	O=$L/t2-r$n; mkdir -p $O
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	if [ $stk = 64 ]; then unset EXP_RX_REGION_STACK; else export EXP_RX_REGION_STACK=$stk; fi
	case $kind in H) mark=Y ;; *) mark=N ;; esac
	echo $mark > $MARK
	./settled.sh apply > $O/apply.txt 2>&1 || die "t2-r$n not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
	[ "$(rx_r $PP/rx_region_stack)" = $stk ] || die "t2-r$n: rx_region_stack $(cat $PP/rx_region_stack), not $stk"
	for p in "enp161s0f0 192.168.1.106" "enp164s0f0 192.168.0.106"; do
		set -- $p; ping -c3 -W2 -I $1 $2 > /dev/null || die "t2-r$n: $1 does not answer after the reopen"
	done
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0; stats > $O/pp.0
	if [ $kind = C ]; then
		( timed_load $O/pmu 5 || exit 1
		  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	fi
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=stk$stk-$kind REGION_SAMPLE=1 WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ wait; die "t2-r$n run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; }
	wait
	stats > $O/pp.1; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	if [ $kind = C ]; then
		perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
		rm -f /dev/shm/zc-pmu.data
	fi
	echo $stk > $O/stack.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	ov=$(join <(awk '{print $1"/"$2, $3}' $O/pp.0 | sort) <(awk '{print $1"/"$2, $3}' $O/pp.1 | sort) | awk '{d[$1] = $3 - $2} END {r = 0; f = 0; for (k in d) {if (k ~ /\/recycle_ring$/) r += d[k]; if (k ~ /recycle_ring_full/) f += d[k]}; printf "to stack %d, overflowing bulks %d (%.3f a page)", r, f, r ? f / r : 0}')
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n stack $stk $kind | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/pmu.log | tail -1 | sed 's/.*: //') | $ov | new kernel warnings $hung"
	[ "$hung" = 0 ] || { diff $O/dmesg.0 $O/dmesg.1 | head -20; die "the kernel warned"; }
	n=$((n + 1))
done
back
