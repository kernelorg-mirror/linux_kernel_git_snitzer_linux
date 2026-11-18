#!/bin/bash
# ab-choice.sh <n>:<Y|N>[:pin0] ...: FLAMEGRAPH round 136, the per-CPU
# transport choice on build 541's client (Mike, 18 November: "client is
# back up on 541, proceed").  The client runs the tree's NFS stack with
# a60960b6a6f99's switch (cbuild132.sh into /root/zc-ckern/T, loaded by
# cswap131.sh); for each run sunrpc.xprt_percpu_choice_ab is set (N: a
# WRITE takes the round-robin transport, as on build 469), one
# prof-write.sh <n> (the WRITE pass with perf, sockets and the
# transports' queues), and the switch back to Y.  pin0: PIN=0, fio's
# writers not pinned.  Printed: the pass, its p99, the transports' queues.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
P=/sys/module/sunrpc/parameters/xprt_percpu_choice_ab
say() { echo "$(date +%T) $*"; }
$SSHC "test -w $P" || { say "the client's sunrpc has no $P"; exit 1; }
for run in "$@"; do
	IFS=: read n v pin <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	$SSHC "echo $v > $P"
	if [ "$pin" = pin0 ]; then PIN=0 ../FLAMEGRAPH/136-write-regression/prof-write.sh $n
	else ../FLAMEGRAPH/136-write-regression/prof-write.sh $n; fi > /dev/null 2>&1
	got=$($SSHC "cat $P; echo Y > $P")
	O=$L/t2-r$n
	say "t2-r$n choice $v${pin:+ $pin} (read $got): $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out)"
	for t in 6 9 12; do say "  t$t queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-$t.txt | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"; done
done
say "ab-choice done"
