#!/bin/bash
# run237.sh: round 64, the KUnit fault hook's flag off the spare counter's
# line.  Two sunrpc.ko built by build-sunrpc.sh from build 514's source on
# this host: A as 514 has it, B with "SUNRPC: keep the fault hook's flag off
# hot lines for the KUnit suites".  Swapped in by swap-sunrpc.sh in turn, A B
# A B (t2-r237..r240), each run a loan run on both ports after the settled
# setting, a trim of the eight and 120 s of rest, sampled as t2-r235 (cycles,
# instructions, L1 and L2 refills on nfsd's CPUs for 8 s).  At the end the
# stock sunrpc is put back, with nothing in updates/.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
put_back() { SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $L/t2-r240/swap-stock.txt 2>&1
	rm -f /lib/modules/$(uname -r)/updates/sunrpc.ko; depmod -a
	SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh >> $L/t2-r240/swap-stock.txt 2>&1
	./settled.sh apply > $L/t2-r240/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), $(tail -1 $L/t2-r240/apply-stock.txt)"; }
n=237
for v in A B A B; do
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	SUNRPC_KO=$L/sunrpc-flag-$v/sunrpc/sunrpc.ko DROP_CLIENTS=1 ./swap-sunrpc.sh > $O/swap.txt 2>&1 ||
		{ say "swap to $v failed: $(tail -3 $O/swap.txt)"; mkdir -p $L/t2-r240; put_back; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "$v not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; mkdir -p $L/t2-r240; put_back; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	( timed_load $O/pmu 5 || exit 1
	  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o $O/pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=pmu-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "$v run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; mkdir -p $L/t2-r240; put_back; exit 1; }
	wait
	perf script -i $O/pmu.data -F ip,sym,period,event 2>/dev/null > $O/pmu.script
	cat /sys/module/sunrpc/sections/.text > $O/sunrpc-text.txt; cat /sys/module/sunrpc/srcversion > $O/sunrpc-srcversion.txt
	w=$(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1)
	say "t2-r$n $v | $(grep '^total' $O/pmu.log) | sunrpc $(cat $O/sunrpc-srcversion.txt) | $w"
	n=$((n + 1))
done
put_back
