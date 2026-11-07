#!/bin/bash
# run241.sh: round 65, the pool's transport queue on a line of its own (S)
# and the handoff of an enqueued transport to the claimed thread (H), each
# against B, build 514's source with round 64's flag change.  The three are
# whole server stacks (sunrpc, lockd, nfsd and the rest) built here by
# build-svcstack.sh, since S and H change types other modules see.  Swapped
# in by swap-sunrpc.sh in turn, B S H B S H (t2-r241..r246), each run a loan
# run on both ports after the settled setting, a trim and 120 s of rest,
# sampled as t2-r235, with the pool's counters around it and the kernel's
# log after it.  At the end the stock stack is put back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
E=$L/t2-r246; mkdir -p $E
put_back() { STACK=none SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $E/swap-stock.txt 2>&1
	./settled.sh apply > $E/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), nfsd $(modinfo -n nfsd), updates: $(ls /lib/modules/$(uname -r)/updates | tr '\n' ' '), $(tail -1 $E/apply-stock.txt)"; }
n=241
for v in B S H B S H; do
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
	  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o $O/pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "$v run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; put_back; exit 1; }
	wait
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	perf script -i $O/pmu.data -F ip,sym,period,event 2>/dev/null > $O/pmu.script
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "$v: the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; put_back; exit 1; }
	n=$((n + 1))
done
put_back
