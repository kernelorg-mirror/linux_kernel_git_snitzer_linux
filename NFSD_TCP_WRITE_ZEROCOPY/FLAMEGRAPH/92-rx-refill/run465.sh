#!/bin/bash
# run465.sh: round 92, what the mid-poll refill costs the rings: run446.sh
# with perf on the ring CPUs (0-7, 56-63) for 8 s from 5 s into twoport's
# load (cycles and instructions at 1999 Hz, kept as pmu.script.xz with
# the CPU), rx_refill_mid 16 against 32 on rings of 128.  rx_refill_mid is a
# run's experiment as EXP_XEU_rx_refill_mid (settled.sh writes and audits
# it); ENTRIES=256 as in run440.sh.  The held mark on in every run for the
# region's exact peak.  Arms as <entries>:<mid>, from t2-r465.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -e /sys/module/xeu/parameters/rx_refill_mid ] || { say "xeu has no rx_refill_mid"; exit 1; }
stop() { echo N > $MARK; unset EXP_XEU_rx_refill_mid; ./settled.sh apply > /dev/null 2>&1; exit 1; }
n=${N0:-465}
for arm in ${ARMS:-128:16 128:32 128:32 128:16 128:16 128:32}; do
	e=${arm%:*} m=${arm#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	export EXP_XEU_rx_refill_mid=$m
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	[ "$(cat /sys/module/xeu/parameters/rx_refill_mid)" = $m ] || { say "rx_refill_mid is not $m"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/net/softnet_stat > $O/softnet.0
	if [ $e = 128 ]; then ent=; else ent="ENTRIES=$e"; fi
	( timed_load $O/pmu 5 || exit 1
	  perf record -q -C 0-7,56-63 -F 1999 -e cycles,instructions -o /dev/shm/zc-ring.data -- sleep 8 > $O/perf.out 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=mid-$e-$m $ent WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	perf script -i /dev/shm/zc-ring.data -F cpu,ip,sym,period,event 2>/dev/null | xz -T8 -3 > $O/pmu.script.xz
	rm -f /dev/shm/zc-ring.data
	dmesg > $O/dmesg.1; cat /proc/net/softnet_stat > $O/softnet.1
	cat /sys/module/xeu/parameters/rx_refill_mid > $O/rx_refill_mid.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n ring $e mid $m | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/pmu.log | tail -1 | sed 's/.*: /peak /') | $(grep -oE 'rings of a \(0-7\) [0-9]+%, rings of b \(56-63\) [0-9]+%' $O/pmu.log) | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
unset EXP_XEU_rx_refill_mid
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt), rx_refill_mid $(cat /sys/module/xeu/parameters/rx_refill_mid)"
