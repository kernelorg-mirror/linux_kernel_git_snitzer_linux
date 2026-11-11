#!/bin/bash
# run440.sh: round 92's step 3, rings of 256 against twoport's 128 on the
# settled setting (stack 96; the client's rpciod and xprtiod strict), no
# perf and no trace.  ENTRIES=256 is a run's experiment (settled.sh audits
# it as EXP_ENTRIES_A/B); the held mark is on in every run for the
# region's exact peak, since a larger ring posts more of its pages
# (footprint rule).  128 256 256 128 128 256 from t2-r440.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
n=${N0:-440}
for e in ${SIZES:-128 256 256 128 128 256}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; echo N > $MARK; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; echo N > $MARK; exit 1; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; echo N > $MARK; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; echo N > $MARK; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/net/softnet_stat > $O/softnet.0
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=ring-$e ENTRIES=$e WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; echo N > $MARK; exit 1; }
	dmesg > $O/dmesg.1; cat /proc/net/softnet_stat > $O/softnet.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n ring $e | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/pmu.log | tail -1 | sed 's/.*: /peak /') | rings $(grep -oE 'rings [0-9.]+%' $O/pmu.log | paste -sd/) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; echo N > $MARK; exit 1; }
	n=$((n + 1))
done
echo N > $MARK
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
