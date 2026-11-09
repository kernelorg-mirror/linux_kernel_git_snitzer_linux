#!/bin/bash
# run355.sh: round 86, the loan table's lines fetched ahead.  sunrpc.ko
# built by build-sunrpc.sh from kernel-7.3/canary.TP ("SUNRPC: Prefetch a
# loaned record's table ahead of its use"), in $L/sunrpc-TP, swapped in
# once; the canary's own nfsd loads against it.  A is
# svc_tcp_rx_table_prefetch=0, B is 3 (both prefetches); the same module
# either way.  A B A B A B with perf (t2-r355..r360), then A and B with the
# held mark for the region's exact peak (t2-r361, t2-r362).  At the end
# the stock sunrpc is put back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
P=/sys/module/sunrpc/parameters
KO=$L/sunrpc-TP/sunrpc/sunrpc.ko
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -z "$(ls /lib/modules/$(uname -r)/updates 2>/dev/null)" ] || { say "updates/ is not empty"; exit 1; }
E=$L/t2-r362; mkdir -p $E
put_back() { rm -f /lib/modules/$(uname -r)/updates/sunrpc.ko; depmod -a
	SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $E/swap-stock.txt 2>&1
	./settled.sh apply > $E/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), updates: $(ls -A /lib/modules/$(uname -r)/updates | tr '\n' ' '), $(tail -1 $E/apply-stock.txt)"; }
client_free || { say "client not free"; exit 1; }
SUNRPC_KO=$KO DROP_CLIENTS=1 ./swap-sunrpc.sh > $L/t2-r355-swap.txt 2>&1 ||
	{ say "swap failed: $(tail -3 $L/t2-r355-swap.txt)"; put_back; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $KO)" ] &&
[ -e $P/svc_tcp_rx_table_prefetch ] || { say "the sunrpc loaded is not $KO"; put_back; exit 1; }
say "module in: sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
n=355
for v in A B A B A B AH BH; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; put_back; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; put_back; exit 1; }
	case $v in *H) mark=Y ;; *) mark=N ;; esac
	echo $mark > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "$v not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; put_back; exit 1; }
	case $v in A*) echo 0 > $P/svc_tcp_rx_table_prefetch ;; B*) echo 3 > $P/svc_tcp_rx_table_prefetch ;; esac
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	if [ $mark = N ]; then
		( timed_load $O/pmu 5 || exit 1
		  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o /dev/shm/zc-pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	fi
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=tp-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "$v run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; put_back; exit 1; }
	wait
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	cat $P/svc_tcp_rx_table_prefetch > $O/prefetch.txt
	if [ $mark = N ]; then
		perf script -i /dev/shm/zc-pmu.data -F ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
		rm -f /dev/shm/zc-pmu.data
	fi
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v prefetch $(cat $O/prefetch.txt) | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+, threads woken [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/pmu.log | sed 's/.*: //') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "$v: the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; put_back; exit 1; }
	n=$((n + 1))
done
put_back
