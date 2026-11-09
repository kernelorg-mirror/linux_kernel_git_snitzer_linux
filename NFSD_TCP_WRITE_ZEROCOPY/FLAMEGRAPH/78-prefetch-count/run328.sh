#!/bin/bash
# run328.sh: round 78, the region count taken in the prefetch's walk.
# Two sunrpc.ko built by build-sunrpc.sh for the canary: A from its own
# source (013b4fb22657's sunrpc), B with "SUNRPC: Count a loaned skb's
# region pages in the walk that prefetches" (kernel-7.3/canary.PC,
# f9eb67570da8).  Swapped in by swap-sunrpc.sh in turn, A B A B A B
# (t2-r328..r333), each run as round 74's.  At the end the stock sunrpc is
# put back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
E=$L/t2-r333; mkdir -p $E
# SUNRPC_KO=none alone leaves updates/sunrpc.ko in place (the stock was not
# put back on 9 November and was by hand): take it out and depmod first.
put_back() { rm -f /lib/modules/$(uname -r)/updates/sunrpc.ko; depmod -a
	SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $E/swap-stock.txt 2>&1
	./settled.sh apply > $E/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), updates: $(ls /lib/modules/$(uname -r)/updates | tr '\n' ' '), $(tail -1 $E/apply-stock.txt)"; }
n=328
for v in A B A B A B; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; put_back; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	KO=$L/sunrpc-PC-$v/sunrpc/sunrpc.ko
	client_free || { say "client not free"; put_back; exit 1; }
	SUNRPC_KO=$KO DROP_CLIENTS=1 ./swap-sunrpc.sh > $O/swap.txt 2>&1 ||
		{ say "swap to $v failed: $(tail -3 $O/swap.txt)"; put_back; exit 1; }
	[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $KO)" ] ||
		{ say "$v: the sunrpc loaded is not $KO"; put_back; exit 1; }
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
	perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
	rm -f /dev/shm/zc-pmu.data
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "$v: the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; put_back; exit 1; }
	n=$((n + 1))
done
put_back
