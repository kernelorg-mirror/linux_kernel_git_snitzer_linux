#!/bin/bash
# run-split-prof.sh: round 104, a profile of svc_rtc_tx_split on both ports
# at the two-port setting (Mike, 16 November: "profile the split on two
# ports"), against the same run with split N.  RUNS is <n>:<split> in turn
# (default "768:N 769:Y").  Each run is run-split2.sh's (read-check.sh's
# two-port read, after a trim and 120 s of rest); 15 s into fio's count
# every CPU is recorded for 8 s at 999 Hz with kernel call chains, and
# rtc_stats and /proc/stat are read either side of it.  Then, in the
# run's directory: cycles by CPU and command, by command and symbol, self
# and with children on the rtc CPUs (8-55), folded stacks and flamegraphs
# for all CPUs and for the rtc CPUs, for difffolded.pl between the two.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
P=/sys/module/sunrpc/parameters
FG=${FG:-/opt/FlameGraph}
grep -qw 'irqchip.gicv3_pseudo_nmi=1' /proc/cmdline || { say "no pseudo-NMI on this boot"; exit 1; }
[ "$(sysctl -n kernel.nmi_watchdog)" = 1 ] || { say "NMI watchdog off"; exit 1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = 8843BD2AFB934E2FEFD942B ] || { say "not the split sunrpc"; exit 1; }
trap 'echo N > $P/svc_rtc_tx_split' EXIT
for run in ${RUNS:-768:N 769:Y}; do
	IFS=: read n split <<< "$run"
	[ "$(df --output=avail -k / | tail -1)" -gt 1572864 ] || { say "root has under 1.5 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O || exit 1
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; [ "${TRIM:-1}" = 0 ] || for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep ${REST:-120}
	dmesg > $O/dmesg.0
	echo $split > $P/svc_rtc_tx_split	# latched by each connection at attach: the mounts below
	say "t2-r$n: two ports, split $(cat $P/svc_rtc_tx_split), profiled"
	cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.0 2>/dev/null
	( W=$O/run/wire-sec.tsv
	  for i in $(seq 600); do [ -f $W ] && [ $(wc -l < $W) -ge 26 ] && break; sleep 0.5; done
	  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w0 2>/dev/null; cat /proc/stat > $O/stat.w0
	  perf record -q -a -g -F 999 -o /dev/shm/zc-split.data -- sleep 8 > $O/perf.out 2>&1
	  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w1 2>/dev/null; cat /proc/stat > $O/stat.w1
	) &
	prof=$!
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
		WARM=${WARM:-10} RUNTIME=${RUNTIME:-40} RW=read BS=1M RSIZE=262144 RWTSV=$O/rw.tsv TAG=splitprof-$n \
		WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1
	rc=$?
	wait $prof
	cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.1 2>/dev/null
	echo N > $P/svc_rtc_tx_split
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|svc_rtc:')
	say "t2-r$n split $split: twoport exit $rc: $(grep '^total' $O/run.log) | new kernel warnings $hung"
	D=/dev/shm/zc-split.data
	if [ -f $D ]; then
		perf report -i $D --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/perf-cpu.txt
		perf report -i $D --no-children -g none --sort comm --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . > $O/perf-comm.txt
		perf report -i $D --no-children -g none --sort comm,sym --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/perf-comm-sym.txt
		perf report -i $D --no-children -g none --sort sym --percent-limit 0.2 --cpu 8-55 2>/dev/null | grep -v '^#' | grep . | head -120 > $O/perf-rtc-self.txt
		perf report -i $D --children -g none --sort sym --percent-limit 1 --cpu 8-55 2>/dev/null | grep -v '^#' | grep . | head -150 > $O/perf-rtc-children.txt
		perf script -i $D 2>/dev/null > /dev/shm/zc-split.script
		$FG/stackcollapse-perf.pl /dev/shm/zc-split.script > $O/all.folded 2>/dev/null
		perf script -i $D --cpu 8-55 2>/dev/null | $FG/stackcollapse-perf.pl > $O/rtc.folded 2>/dev/null
		grep -v -E 'cpu_idle|do_idle|default_idle' $O/all.folded | $FG/flamegraph.pl --title "t2-r$n split $split, all CPUs" > $O/all.svg 2>/dev/null
		grep -v -E 'cpu_idle|do_idle|default_idle' $O/rtc.folded | $FG/flamegraph.pl --title "t2-r$n split $split, CPUs 8-55" > $O/rtc.svg 2>/dev/null
		rm -f /dev/shm/zc-split.script $D
	fi
	grep -E '^  read:|^port |drives of' $O/run.log | sed 's/^/    /'
	./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
	[ $rc = 0 ] || exit $rc
done
