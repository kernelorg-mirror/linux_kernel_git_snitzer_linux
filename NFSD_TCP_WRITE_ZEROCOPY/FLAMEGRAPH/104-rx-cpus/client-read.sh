#!/bin/bash
# client-read.sh <n>: round 104, the client's side of an NFS READ (Mike, 16
# November: "focus on how to optimize the client side while running settled
# settings on the server (without split)").  A quick two-port read strobe at
# the settled setting (no trim, REST=30, WARM=5 RUNTIME=25, fio 1 MiB at
# rsize 256 KiB, eight drives); any twoport.sh knob in the environment
# (NCONN, JOBS, IODEPTH, RSIZE, PIN, CLI_SCOPE, CLI_STRICT, QDISC, ...)
# passes through, and twoport.sh puts the client back after the run.  In the
# window, on the client: /proc/self/mountstats before and after (each READ's
# queue, round trip and execute time; each transport's queues), 1 s of the
# sunrpc:rpc_stats_latency tracepoint, mpstat per CPU for 5 s and a 5 s
# profile; on both ends ten ss samples of every NFS socket.  Then
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
T=/sys/kernel/tracing
( W=$O/run/wire-sec.tsv
  for i in $(seq 600); do [ -f $W ] && [ $(wc -l < $W) -ge ${ROW:-20} ] && break; sleep 0.5; done
  $SSHC "cat /proc/self/mountstats" > $O/cli-mountstats.0
  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w0 2>/dev/null
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
  $SSHC "cat /proc/self/mountstats" > $O/cli-mountstats.1
  cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.w1 2>/dev/null
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep ." > $O/cli-perf-cpu.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort comm --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep ." > $O/cli-perf-comm.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --no-children -g none --sort sym --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -120" > $O/cli-perf-self.txt
  $SSHC "perf report -i /dev/shm/zc-cli.data --children -g none --sort sym --percent-limit 1 2>/dev/null | grep -v '^#' | grep . | head -150" > $O/cli-perf-children.txt
  $SSHC "rm -f /dev/shm/zc-cli.data"
) &
prof=$!
say "t2-r$n: client read strobe ${NCONN:+nconnect $NCONN }${JOBS:+jobs $JOBS }${IODEPTH:+iodepth $IODEPTH }"
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
./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
exit $rc
