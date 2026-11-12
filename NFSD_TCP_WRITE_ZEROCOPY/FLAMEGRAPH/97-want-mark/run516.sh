#!/bin/bash
# run516.sh: round 97's second step, the held buffer at 384 KiB (RCVBUF)
# with the want mark at 2 against 6, in turns (2 6 2 6), for the footprint
# at the rate: round 56 had 61.9 GB/s at 384 KiB with the mark at 6.
# Otherwise run510.sh: the A/B of the want mark (svc_tcp_rx_want_eighths,
# the eighths of the open window an incomplete loaned record waits for
# before a thread takes it up; 6 settled since round 25) by the client's
# round trip, the rate and the held pages, on the settled two-port loan
# setting.  SWEEP is the marks in turn (6 4 2 6 4 2), each a twoport.sh
# run with WANT_EIGHTHS=<mark> (twoport sets it for the run and puts the
# settled 6 back) and round 96's rttsplit.bt for 12 s of the window.
# RUNS from t2-r510.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/96-server-rtt
MARKS=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/97-want-mark
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/parameters/svc_tcp_rx_target_window)" = 0 ] || { say "the window is not 0"; exit 1; }
stop() { echo N > $MARK; ./settled.sh apply > /dev/null 2>&1; exit 1; }
# From the window's start (twoport writes perf-stat-window.txt as it opens
# it): the trace on CPU 63 with the region's sampler.
split() {	# split <run dir>
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	taskset -c 63 timeout -s INT 12 bpftrace $HERE/$BT.bt > $O/$BT.out 2> $O/$BT.err
	date +%s.%N > $O/$BT.t1
	wait
}
BT=${BT:-rttsplit}
n=${N0:-516}
for w in ${SWEEP:-2 6 2 6}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	split $O &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=rcv${RCVBUF:-393216}-mark-$w RCVBUF=${RCVBUF:-393216} WANT_EIGHTHS=$w WRITES=$TWO_WRITES REGION_SAMPLE=1 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n rcvbuf ${RCVBUF:-393216} mark $w | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt) | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	python3 -I $HERE/rttsplit-read.py t2-r$n 2>&1 | sed 's/^/    /'
	python3 -I $MARKS/markread.py t2-r$n 2>&1 | sed 's/^/    /'
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
