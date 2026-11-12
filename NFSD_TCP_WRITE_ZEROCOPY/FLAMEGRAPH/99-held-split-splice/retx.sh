#!/bin/bash
# retx.sh: round 99, what the client's retransmits are at window 7.  For
# each svc_tcp_rx_target_window in SWEEP (default "0 7"), budget WRITES
# (default 64), one settled two-port run, and over twoport's window
# (from perf-stat-window.txt, RUNTIME s): the client's and the server's
# /proc/net/netstat and /proc/net/snmp at its start and end (every TCP
# counter: tail loss probes, RTOs, fast and spurious retransmits,
# DSACKs, window probes), and the client's ss -tin of its connections
# to the server every second (timers, retrans, backoff, probes).  Read
# by retx-read.py.  The window is left at 0.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/99-held-split-splice
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
SP=/sys/module/sunrpc/parameters
RUNTIME=40
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -e $SP/svc_tcp_rx_target_window ] || { say "this sunrpc has no svc_tcp_rx_target_window"; exit 1; }
stop() { echo N > $MARK; echo 0 > $SP/svc_tcp_rx_target_window; ./settled.sh apply > /dev/null 2>&1; exit 1; }
snap() {	# snap <file prefix>: both hosts' counters
	cat /proc/net/netstat /proc/net/snmp > $1.srv
	$SSHC "cat /proc/net/netstat /proc/net/snmp" < /dev/null > $1.cli
}
collect() {	# collect <run dir>
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	snap $O/net0
	$SSHC "for i in \$(seq $RUNTIME); do echo \"# \$(date +%s.%N)\"; ss -tinH state established '( dport = :2049 )'; sleep 1; done" < /dev/null > $O/cli-ss.txt 2>&1 &
	sleep $RUNTIME
	snap $O/net1
	wait
}
n=${N0:-551}
for w in ${SWEEP:-0 7}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	echo $w > $SP/svc_tcp_rx_target_window
	collect $O &
	env WARM=10 RUNTIME=$RUNTIME TSV=$O/results.tsv TAG=retx-$w-b${WRITES:-64} WRITES=${WRITES:-64} REGION_SAMPLE=1 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	echo 0 > $SP/svc_tcp_rx_target_window
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n window $w budget ${WRITES:-64} | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung | $(grep '^client retransmits' $O/pmu/window.txt)"
	python3 -I $HERE/retx-read.py $O 2>&1 | sed 's/^/    /'
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
