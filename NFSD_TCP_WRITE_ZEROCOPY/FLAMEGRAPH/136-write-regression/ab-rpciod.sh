#!/bin/bash
# ab-rpciod.sh <n>:<Y|N> ...: FLAMEGRAPH round 136, rpciod per-CPU or
# unbound on build 541's client (Mike, 18 November: "start with the
# rpciod per-CPU switch, I would like to preserve these behind a
# modparam in case they are critical to IOPS or READ perf").  The
# client runs the tree's NFS stack from the tip, with 298c732504785's
# sunrpc.rpciod_per_cpu, a load-time parameter: for each run the
# client's /etc/modprobe.d/zc-rpciod.conf is written (N) or removed (Y),
# the stack loaded again (cswap131.sh), the parameter and rpciod's
# per_cpu read back, then one prof-write.sh <n>.  An N run is held with
# EXP_CLI_WQ="rpciod/per_cpu=0", and twoport.sh holds the unbound rpciod
# affinity_strict to its node, as it did on build 469.  The conf is
# removed at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
for run in "$@"; do
	IFS=: read n v <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	if [ "$v" = N ]; then $SSHC "echo 'options sunrpc rpciod_per_cpu=N' > /etc/modprobe.d/zc-rpciod.conf"; w=0
	else $SSHC "rm -f /etc/modprobe.d/zc-rpciod.conf"; w=1; fi
	$SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/T/root/ 2>&1 | tail -1" > /dev/null
	got=$($SSHC "cat /sys/module/sunrpc/parameters/rpciod_per_cpu /sys/devices/virtual/workqueue/rpciod/per_cpu | paste -sd/")
	[ "$got" = "$v/$w" ] || { say "t2-r$n: rpciod_per_cpu/per_cpu read $got, want $v/$w"; exit 1; }
	EXP_CLI_WQ="rpciod/per_cpu=$w" ../FLAMEGRAPH/136-write-regression/prof-write.sh $n > /dev/null 2>&1
	O=$L/t2-r$n
	say "t2-r$n rpciod_per_cpu $v (read $got, rpciod $(grep -oE 'rpciod and xprtiod strict [^ ]+' $O/run/geometry.txt 2>/dev/null)): $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out)"
	for t in 6 9 12; do say "  t$t queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-$t.txt | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"; done
done
$SSHC "rm -f /etc/modprobe.d/zc-rpciod.conf"
say "ab-rpciod done"
