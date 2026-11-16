#!/bin/bash
# read-prof.sh <n>: the profile of a READ strobe, to choose the next READ
# round (Mike, 16 November: "Run a profile run to find what the focus should
# be on the next READ round ... collected from the READ strobe").  It is
# client-read.sh with, in the same window, 8 s of every server CPU at 999 Hz
# with kernel call chains (perf.data kept as perf-srv.data.xz), then the
# server reports: cycles by CPU, by command and symbol, the rings (0-7,
# 56-63) and the rtc CPUs (8-55) self and with children, folded stacks and
# flamegraphs.  What follows is client-read.sh.
# client-read.sh: round 104, the client's side of an NFS READ
# November: "focus on how to optimize the client side while running settled
# settings on the server (without split)").  A quick two-port read strobe at
# the settled setting (no trim, REST=30, WARM=5 RUNTIME=25, fio 1 MiB at
# rsize 256 KiB, eight drives); any twoport.sh knob in the environment
# (NCONN, JOBS, IODEPTH, RSIZE, PIN, CLI_SCOPE, CLI_STRICT, QDISC, ...)
# passes through, and twoport.sh puts the client back after the run.  In the
# window, on the client: /proc/self/mountstats before and after (each READ's
# queue, round trip and execute time; each transport's queues), 1 s of the
# sunrpc:rpc_stats_latency tracepoint, mpstat per CPU for 5 s and a 5 s
# profile; on both ends ten ss samples of every NFS socket; on the server
# /proc/diskstats either side (each drive's commands and their time), and
# with SRV=rtcread the server's own stages (rtcread.bt).  Then
# client-read.py summarises it into OUT/summary-client.txt.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
n=${1:?run number}
H=$(cd ../FLAMEGRAPH/104-rx-cpus && pwd)
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ ! -e /sys/module/sunrpc/parameters/svc_rtc_tx_split ] || { say "a sunrpc with the split is loaded"; exit 1; }
O=$L/t2-r$n; mkdir -p $O || exit 1
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
sync; [ "${TRIM:-0}" = 0 ] || for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep ${REST:-30}
dmesg > $O/dmesg.0
# RECVB=<n>: sunrpc's svc_rtc_recv_budget for the run, put back after it
RB=/sys/module/sunrpc/parameters/svc_rtc_recv_budget
if [ -n "${RECVB:-}" ]; then
	[ -e $RB ] || { say "no svc_rtc_recv_budget in this sunrpc"; exit 1; }
	rb0=$(cat $RB); trap 'echo $rb0 > $RB' EXIT
	echo $RECVB > $RB || exit 1
fi
[ -e $RB ] && echo "svc_rtc_recv_budget $(cat $RB)" > $O/recv-budget.txt
T=/sys/kernel/tracing
( W=$O/run/wire-sec.tsv
  for i in $(seq 600); do [ -f $W ] && [ $(wc -l < $W) -ge ${ROW:-20} ] && break; sleep 0.5; done
  $SSHC "cat /proc/self/mountstats" > $O/cli-mountstats.0
  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w0 2>/dev/null
  { date +%s.%N; cat /proc/diskstats; } > $O/diskstats.w0
  # SRV=rtcread: where the server's own time goes (rtcread.bt, 5 s; it
  # costs about 0.7 GB/s, r739)
  [ "${SRV:-}" = rtcread ] && bpftrace -q $H/rtcread.bt > $O/rtcread.txt 2>&1 &
  cat /proc/stat > $O/stat.w0
  perf record -q -a -g -F 999 -o /dev/shm/zc-rprof.data -- sleep 8 > $O/perf.out 2>&1 &
  $SSHC "mpstat -P ALL 1 5" > $O/cli-mpstat-w.txt 2>&1 &
  $SSHC "perf record -q -a -g -F 499 -o /dev/shm/zc-cli.data -- sleep 5 > /dev/null 2>&1" &
  for k in $(seq 10); do
	echo "# $k $(date +%s.%N)" >> $O/cli-ss.txt
	$SSHC "ss -tnmi state established '( dport = :2049 )'" >> $O/cli-ss.txt
	echo "# $k $(date +%s.%N)" >> $O/srv-ss.txt
	ss -tnmi state established '( sport = :2049 )' >> $O/srv-ss.txt
	sleep 0.4
  done
  $SSHC "echo 1 > $T/events/sunrpc/rpc_stats_latency/enable; echo > $T/trace; sleep 1; echo 0 > $T/events/sunrpc/rpc_stats_latency/enable; cat $T/trace" > $O/cli-latency.trace
  wait
  cat /proc/stat > $O/stat.w1
  $SSHC "cat /proc/self/mountstats" > $O/cli-mountstats.1
  { date +%s.%N; cat /proc/diskstats; } > $O/diskstats.w1
  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w1 2>/dev/null
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep ." > $O/cli-perf-cpu.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort comm --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep ." > $O/cli-perf-comm.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort sym --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -120" > $O/cli-perf-self.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --children -g none --sort sym --percent-limit 1 2>/dev/null | grep -v '^#' | grep . | head -150" > $O/cli-perf-children.txt
  $SSHC "rm -f /dev/shm/zc-cli.data"
) &
prof=$!
say "t2-r$n: client read strobe ${RECVB:+recv budget $RECVB }${NCONN:+nconnect $NCONN }${JOBS:+jobs $JOBS }${IODEPTH:+iodepth $IODEPTH }"
env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
	WARM=${WARM:-5} RUNTIME=${RUNTIME:-25} RW=read BS=${BS:-1M} RSIZE=${RSIZE:-262144} RWTSV=$O/rw.tsv TAG=client-$n \
	WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1
rc=$?
wait $prof
dmesg > $O/dmesg.1
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|svc_rtc:')
say "t2-r$n: twoport exit $rc: $(grep '^total' $O/run.log) | new kernel warnings $hung"
grep -E '^  read:' $O/run.log | sed 's/^/    /'
python3 -I $H/client-read.py $O > $O/summary-client.txt 2>&1; cat $O/summary-client.txt
FG=${FG:-/opt/FlameGraph}
D=/dev/shm/zc-rprof.data
if [ -f $D ]; then
	perf report -i $D --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/perf-cpu.txt
	perf report -i $D --no-children -g none --sort comm,sym --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/perf-comm-sym.txt
	for g in rings:0-7,56-63 rtc:8-55; do
		perf report -i $D --no-children -g none --sort sym --percent-limit 0.2 --cpu ${g#*:} 2>/dev/null | grep -v '^#' | grep . | head -120 > $O/perf-${g%%:*}-self.txt
		perf report -i $D --children -g none --sort sym --percent-limit 1 --cpu ${g#*:} 2>/dev/null | grep -v '^#' | grep . | head -150 > $O/perf-${g%%:*}-children.txt
		perf script -i $D --cpu ${g#*:} 2>/dev/null | $FG/stackcollapse-perf.pl > $O/${g%%:*}.folded 2>/dev/null
		grep -v -E 'cpu_idle|do_idle|default_idle' $O/${g%%:*}.folded | $FG/flamegraph.pl --title "t2-r$n READ strobe, ${g%%:*} CPUs (${g#*:})" > $O/${g%%:*}.svg 2>/dev/null
	done
	xz -T8 -3 -c $D > $O/perf-srv.data.xz; rm -f $D
fi
./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
exit $rc
