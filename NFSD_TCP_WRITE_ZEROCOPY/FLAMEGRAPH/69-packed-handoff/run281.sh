#!/bin/bash
# run281.sh: round 69, whether the handoff lets the pool's read-mostly
# fields share the queue's line.  Two whole server stacks built here by
# build-svcstack.sh from build 514's source with round 64's flag change:
# SH (the queue on a line of its own, 40 bytes of padding, and the handoff)
# and PH (the thread counts and sp_rtc_rqsts back on the queue's line, the
# idle list and sp_flags on the next, 256 bytes, and the handoff).  Swapped
# in by swap-sunrpc.sh in turn, SH PH SH PH SH PH (t2-r281..r286), each run
# as round 68's (perf data in /dev/shm).  At the end the stock stack is put
# back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
E=$L/t2-r286; mkdir -p $E
put_back() { STACK=none SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $E/swap-stock.txt 2>&1
	./settled.sh apply > $E/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), nfsd $(modinfo -n nfsd), updates: $(ls /lib/modules/$(uname -r)/updates | tr '\n' ' '), $(tail -1 $E/apply-stock.txt)"; }
n=281
for v in SH PH SH PH SH PH; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; put_back; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; put_back; exit 1; }
	STACK=$L/svcstack-pool-$v/stack SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $O/swap.txt 2>&1 ||
		{ say "swap to $v failed: $(tail -3 $O/swap.txt)"; put_back; exit 1; }
	[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $L/svcstack-pool-$v/stack/sunrpc.ko)" ] &&
	[ "$(cat /sys/module/nfsd/srcversion)" = "$(modinfo -F srcversion $L/svcstack-pool-$v/stack/nfsd.ko)" ] ||
		{ say "$v: the stack is not the one loaded"; put_back; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "$v not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; put_back; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	( timed_load $O/pmu 5 || exit 1
	  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "$v run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; put_back; exit 1; }
	wait
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null > $O/pmu.script
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "$v: the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; put_back; exit 1; }
	n=$((n + 1))
done
put_back
