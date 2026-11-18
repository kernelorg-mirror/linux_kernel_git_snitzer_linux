#!/bin/bash
# ab-counts.sh <n>:<Y|N> ...: FLAMEGRAPH round 131, the client's
# call_start counters (Mike, 18 November: "do step 1").  The client runs
# this tree's NFS stack (cbuild131.sh, loaded by cswap.sh); for each run
# the client's sunrpc.rpc_call_counts_ab is set (N skips the two
# increments), one settled-iops.sh run of nfs4k.sh <n>, the switch back
# to Y.  Printed: IOPS, each host's CPU a READ, the client's spread.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
L=${LOGS:-/var/tmp/zc-run-logs/r131}
P=/sys/module/sunrpc/parameters/rpc_call_counts_ab
mkdir -p $L
say() { echo "$(date +%T) $*"; }
$SSHC "test -w $P" || { say "the client's sunrpc has no $P"; exit 1; }
for run in "$@"; do
	IFS=: read n v <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	$SSHC "echo $v > $P"
	./step.sh $L counts-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	got=$($SSHC "cat $P; echo Y > $P")
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v v=$v -v g=$got -v rc=$rc '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s counts %s (read %s at the end) rc %s: server %.2f us, client %.2f us of CPU a READ\n", n, v, g, rc, s / 100 * 64 / m, c / 100 * 48 / m }'
	awk -v n=$n '$1 == "Average:" && $2 ~ /^[0-9]+$/ { b[++k] = 100 - $NF - $6 } END { asort(b); printf "t2-r%s client CPUs min %.0f median %.0f max %.0f\n", n, b[1], b[int(k/2)], b[k] }' /var/tmp/zc-run-logs/t2-r$n/run/cli-mpstat.txt
	say "after $n: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
done
