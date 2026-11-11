#!/bin/bash
# run479.sh: round 93, the receive region's pages held through a run and
# the payload's way through the L3, on the settled two-port setting
# (rings of 128, stack 96, the client's rpciod and xprtiod strict), at
# rx_refill_mid 16 (settled on tardis2) and 0 in turns.  twoport.sh with
# REGION_SAMPLE=1 samples the pages held every 20 ms beside the kernel's
# exact peak (the held mark on); over twoport's 10 s window the mesh's
# requests to the memory controller and its cache fills are counted
# (arm_cmn, two events, all the mesh can count at once).  No perf on the
# cores and no trace.  ARMS are rx_refill_mid values, from t2-r479.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/93-l3-working-set
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -e /sys/module/xeu/parameters/rx_refill_mid ] || { say "xeu has no rx_refill_mid"; exit 1; }
[ -e /sys/bus/event_source/devices/arm_cmn_0 ] || { say "arm_cmn not loaded"; exit 1; }
stop() { echo N > $MARK; unset EXP_XEU_rx_refill_mid; ./settled.sh apply > /dev/null 2>&1; exit 1; }
# The mesh over twoport's window: wait for the window's perf stat to start
# (twoport writes perf-stat-window.txt as it opens the window), then count
# the same 10 s, with the epoch at its start for the region's samples.
mesh() {	# mesh <run dir>
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	date +%s.%N > $O/mesh.t0
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mesh.txt -- sleep 10 > /dev/null 2>&1
}
n=${N0:-479}
for m in ${ARMS:-16 0 0 16 16 0}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	export EXP_XEU_rx_refill_mid=$m
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	[ "$(cat /sys/module/xeu/parameters/rx_refill_mid)" = $m ] || { say "rx_refill_mid is not $m"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	mesh $O &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=l3-$m WRITES=128 REGION_SAMPLE=1 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	dmesg > $O/dmesg.1
	cat /sys/module/xeu/parameters/rx_refill_mid > $O/rx_refill_mid.txt
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n mid $m | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | $(python3 -I $HERE/l3read.py t2-r$n 2>&1 | cut -d'|' -f2- | tr -s ' ') | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt) | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
unset EXP_XEU_rx_refill_mid
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt), rx_refill_mid $(cat /sys/module/xeu/parameters/rx_refill_mid)"
