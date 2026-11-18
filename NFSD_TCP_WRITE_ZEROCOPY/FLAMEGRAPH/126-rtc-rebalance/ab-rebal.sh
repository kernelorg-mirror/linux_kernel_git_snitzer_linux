#!/bin/bash
# ab-rebal.sh <n>:<ms> ...: FLAMEGRAPH round 126, the rtc rebalancer
# (Mike, 18 November: "implement the rebalancer after r1033, quick A/B").
# For each run: sunrpc's svc_rtc_rebalance_ms <ms> (0 off), one
# settled-iops.sh run of nfs4k.sh <n>, svc_rtc_rebalance_ms back to 0.
# Printed: IOPS, server CPU a READ, the work and ring CPUs' busy spread,
# the moves (rtc_stats "rebalanced").
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r126}
P=/sys/module/sunrpc/parameters/svc_rtc_rebalance_ms
S=/sys/kernel/debug/nfsd/rtc_stats
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the round's sunrpc"; exit 1; }
for run in "$@"; do
	IFS=: read n ms <<< "$run"
	[ -n "$n" ] && [ -n "$ms" ] || { say "ab-rebal.sh <n>:<ms> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	m0=$(awk '$1 == "rebalanced" {print $2}' $S)
	echo $ms > $P
	./step.sh $L rebal-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	echo 0 > $P
	m1=$(awk '$1 == "rebalanced" {print $2}' $S)
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v ms=$ms -v rc=$rc -v mv=$((m1 - m0)) '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy" && !b) b = $(i + 1) } sub("%", "", b); printf "t2-r%s rebalance_ms %s rc %s: %.2f us of server CPU a READ, %d moves\n", n, ms, rc, b / 100 * 64 / m, mv }'
	awk -v n=$n '$1 == "Average:" && $2 ~ /^[0-9]+$/ { b = 100 - $NF; c = $2 + 0; if (c >= 8 && c <= 55) w[++nw] = b; else r[++nr] = b }
	END { asort(w); asort(r); printf "t2-r%s work CPUs min %.0f p25 %.0f median %.0f p75 %.0f max %.0f; ring CPUs min %.0f median %.0f max %.0f\n", n, w[1], w[int(nw/4)], w[int(nw/2)], w[int(3*nw/4)], w[nw], r[1], r[int(nr/2)], r[nr] }' /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt
	say "after $n: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt), rebalance_ms $(cat $P)"
done
