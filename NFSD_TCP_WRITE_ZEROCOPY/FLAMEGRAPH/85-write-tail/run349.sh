#!/bin/bash
# run349.sh: round 85, three pairs with staying off (A) and on (B),
# t2-r349..r354 in the order A B A B A B, each with tail-sgl.bt for five
# seconds in its window and the client's mountstats copied at its start and
# end.  Round 82's stack ($L/svcstack-ST) swapped in once; the stock stack
# put back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
P=/sys/module/sunrpc/parameters
STK=$L/svcstack-ST/stack
BT=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/85-write-tail/tail-sgl.bt
MS=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/84-reply-time/ms.py
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -z "$(ls /lib/modules/$(uname -r)/updates 2>/dev/null)" ] || { say "updates/ is not empty"; exit 1; }
E=$L/t2-r354; mkdir -p $E
put_back() { STACK=none SUNRPC_KO=none DROP_CLIENTS=1 ./swap-sunrpc.sh > $E/swap-stock.txt 2>&1
	./settled.sh apply > $E/apply-stock.txt 2>&1
	say "stock: sunrpc $(cat /sys/module/sunrpc/srcversion), $(modinfo -n sunrpc), updates: $(ls -A /lib/modules/$(uname -r)/updates | tr '\n' ' '), $(tail -1 $E/apply-stock.txt)"; }
client_free || { say "client not free"; exit 1; }
SUNRPC_KO=none STACK=$STK DROP_CLIENTS=1 ./swap-sunrpc.sh > $L/t2-r349-swap.txt 2>&1 ||
	{ say "swap failed: $(tail -3 $L/t2-r349-swap.txt)"; put_back; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $STK/sunrpc.ko)" ] &&
[ -e $P/svc_tcp_rx_stay_us ] || { say "the stack loaded is not $STK"; put_back; exit 1; }
say "stack in: sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
n=349
for v in A B A B A B; do
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; put_back; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "$v not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; put_back; exit 1; }
	case $v in A) echo 0 > $P/svc_tcp_rx_stay_us ;; B) echo 1000 > $P/svc_tcp_rx_stay_us ;; esac
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	( timed_load $O/pmu 5 || exit 1
	  $SSHC cat /proc/self/mountstats < /dev/null > $O/ms0.txt
	  bpftrace $BT > $O/tail-sgl.txt 2>&1
	  $SSHC cat /proc/self/mountstats < /dev/null > $O/ms1.txt ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=bt-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "$v run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; put_back; exit 1; }
	wait
	dmesg > $O/dmesg.1; cat $P/svc_tcp_rx_stay_us > $O/stay_us.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v stay_us $(cat $O/stay_us.txt) | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+, threads woken [0-9]+' $O/pmu.log | head -1) | writes timed $(grep -oE "^@writes: [0-9]+" $O/tail-sgl.txt | grep -oE "[0-9]+$"), over 2 ms $(grep -oE "^@write_over_2ms: [0-9]+" $O/tail-sgl.txt | grep -oE "[0-9]+$") | client $($MS $O/ms0.txt $O/ms1.txt) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "$v: the kernel warned; stopping"; put_back; exit 1; }
	n=$((n + 1))
done
put_back
