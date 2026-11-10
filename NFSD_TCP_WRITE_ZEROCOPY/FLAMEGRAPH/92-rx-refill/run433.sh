#!/bin/bash
# run433.sh: round 92, the receive refill path on tardis2.  Runs as round
# 90's on the settled setting (stack 96; the client's rpciod and xprtiod
# strict), no perf.  A run marked T traces refill.bt for 10 s from the
# start of twoport's window (ss-client.txt), the window its ring drops
# are counted over; U is the same run untraced, to see what the trace
# itself costs the rings.  T U T from t2-r433.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/92-rx-refill
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
n=${N0:-433}
for k in ${KINDS:-T U T}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=refill-$k WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 &
	tp=$!
	if [ $k = T ]; then
		for i in $(seq 1 300); do [ -e $O/pmu/ss-client.txt ] && break; kill -0 $tp 2>/dev/null || break; sleep 1; done
		[ -e $O/pmu/ss-client.txt ] && bpftrace $HERE/refill.bt 10 > $O/refill.txt 2>&1
	fi
	wait $tp || { say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; exit 1; }
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $k | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | far $(grep "far node" $O/pmu.log | grep -oE '[0-9.]+%' | paste -sd/) | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
	n=$((n + 1))
done
