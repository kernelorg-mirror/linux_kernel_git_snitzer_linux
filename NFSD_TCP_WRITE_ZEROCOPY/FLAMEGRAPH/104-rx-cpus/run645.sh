#!/bin/bash
# run645.sh: round 104, what the receive rings' CPUs do on two-port
# reads at the settled xps_rxqs (round 103: 81 to 93% busy, all in
# softirq).  RUNS is <n>:<rw> in turn.  In each run's window it profiles
# the sixteen ring CPUs (0-7, 56-63) with call graphs for 8 s and reads
# each port's per-ring counters and /proc/softirqs either side of it.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
snap() {	# snap <dir> <tag>
	for d in enp161s0f0 enp164s0f0; do ethtool -S $d > $1/ethtool-$d.$2; done
	cat /proc/softirqs > $1/softirqs.$2; date +%s.%N > $1/time.$2
}
for run in ${RUNS:-645:read 646:write}; do
	IFS=: read n rw <<< "$run"
	[ "$(df --output=avail -k / | tail -1)" -gt 1572864 ] || { say "root has under 1.5 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	( sleep 75; snap $O 0
	  perf record -q -g -C 0-7,56-63 -F 1999 -o /dev/shm/zc-rx.data -- sleep 8 > $O/perf.out 2>&1
	  snap $O 1 ) &
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
		WARM=10 RUNTIME=40 RW=$rw RWTSV=$O/rw.tsv TAG=8d-ab-$rw-rxcpu \
		WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; wait; exit 1; }
	wait
	dmesg > $O/dmesg.1
	perf report -i /dev/shm/zc-rx.data --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/perf-cpu.txt
	perf report -i /dev/shm/zc-rx.data --no-children -g none --percent-limit 0.3 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -80 > $O/perf-rx-self.txt
	perf report -i /dev/shm/zc-rx.data --children -g none --percent-limit 2 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -100 > $O/perf-rx-children.txt
	perf report -i /dev/shm/zc-rx.data --no-children -g caller,0.5,callee --percent-limit 1.5 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -600 > $O/perf-rx-callers.txt
	perf script -i /dev/shm/zc-rx.data -F cpu,ip,sym 2>/dev/null | xz -T8 -3 > $O/perf-g.script.xz
	rm -f /dev/shm/zc-rx.data
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n ab $rw | $(grep '^total' $O/run.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/run.log | awk '{s+=$3} END {print s}') | $(grep -o 'no drive stepped down' $O/run.log || echo 'a drive stepped down') | new kernel warnings $hung"
	grep -E '^  (read|write):|^port |drives of' $O/run.log | sed 's/^/    /'
	echo "    ring CPUs busy: $(awk '$2 ~ /^[0-9]+$/ && ($2 < 8 || $2 > 55) {u[$2] += 100 - $NF; c[$2]++} END {for (k in u) printf "%s:%.0f ", k, u[k]/c[k]}' $O/run/srv-mpstat.txt)"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
