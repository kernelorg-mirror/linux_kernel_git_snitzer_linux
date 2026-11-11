#!/bin/bash
# run494.sh: round 94's window of loaned WRITEs for each export
# (DESIGN-per-export-window.md): the server's module stack built from the
# tip by build-svcstack.sh (the new fields in svc_sock and the record's
# state give most of sunrpc's exports new versions, so lockd, auth_rpcgss
# and the rest are rebuilt with it) is swapped in (swap-sunrpc.sh STACK=,
# which refuses while a client is connected), then one two-port run for
# each svc_tcp_rx_target_window in SWEEP (0: no window), budget 128, with
# heldsplit.bt, sockq.py and the mesh beside each as run490.sh, and the
# window's counters (target_paused, target_unkeyed) from the budget's
# stats line.  The window is left at 0; the modules are left in place.
# From run490.sh:
#
# run485.sh: round 94, where the receive region's held pages sit, on the
# settled two-port setting (rings of 128, stack 96, rx_refill_mid 16).
# Beside each twoport.sh run (REGION_SAMPLE=1, the held mark on): from the
# window's start, heldsplit.bt for 14 s (a loaned record's pages by stage)
# and sockq.py for 10 s (the bytes in nfsd's sockets' receive queues).  No
# perf on the cores.  RUNS runs from t2-r485.  BT=writesplit runs
# writesplit.bt in heldsplit.bt's place (the WRITE's time split between
# XFS and the drive), read by writesplit-read.py.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/94-held-split
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
stop() { echo N > $MARK; echo 0 > $SP/svc_tcp_rx_target_window; ./settled.sh apply > /dev/null 2>&1; exit 1; }
# From the window's start (twoport writes perf-stat-window.txt as it opens
# it): the trace and the socket sampler, both on CPU 63 with the region's
# sampler.  The trace's span is its traced_us back from its end.
split() {	# split <run dir>
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	taskset -c 63 python3 -I $HERE/sockq.py $O/sockq.txt 10 50 &
	taskset -c 63 timeout -s INT 14 bpftrace $HERE/$BT.bt > $O/$BT.out 2>&1
	date +%s.%N > $O/$BT.t1
	wait
}
mesh() {	# mesh <run dir>, as round 93's run479.sh
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	date +%s.%N > $O/mesh.t0
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mesh.txt -- sleep 10 > /dev/null 2>&1
}
BT=${BT:-heldsplit}
SP=/sys/module/sunrpc/parameters
tstat() { tr ' ' '\n' < $SP/svc_tcp_rx_budget_stats | awk -v k=$1 'p == k {print; exit} {p = $0}'; }
if [ -z "$NOSWAP" ]; then
	SUNRPC_KO=none STACK=$L/svcstack-$(uname -r)/stack ./swap-sunrpc.sh > $L/swap-target-window.txt 2>&1 ||
		{ say "swap failed: $(tail -3 $L/swap-target-window.txt)"; exit 1; }
	say "swapped: $(tail -3 $L/swap-target-window.txt | head -1)"
fi
[ -e $SP/svc_tcp_rx_target_window ] || { say "this sunrpc has no svc_tcp_rx_target_window"; exit 1; }
n=${N0:-494}
for w in ${SWEEP:-0 5 4}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	echo $w > $SP/svc_tcp_rx_target_window
	P0=$(tstat target_paused); U0=$(tstat target_unkeyed)
	split $O &
	mesh $O &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=window-$w WRITES=128 REGION_SAMPLE=1 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	echo "window $w: paused $(( $(tstat target_paused) - P0 )) unkeyed $(( $(tstat target_unkeyed) - U0 )) inflight now $(tstat target_inflight)" > $O/target.txt
	say "t2-r$n window $w | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt) | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	python3 -I $HERE/$BT-read.py t2-r$n 2>&1 | sed 's/^/    /'
	echo "    $(grep '^loans' $O/pmu/window.txt); $(cat $O/target.txt)"
	echo "    $(python3 -I $HERE/../93-l3-working-set/l3read.py t2-r$n 2>&1 | cut -d'|' -f3- | tr -s ' ')"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
echo 0 > $SP/svc_tcp_rx_target_window
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
