#!/bin/bash
# ab-partial.sh <n>:<Y|N> ...: FLAMEGRAPH round 129, waiting for the rest
# of a call header on the rtc work item (Mike, 18 November: "implement
# the no_peek fix, quick A/B").  For each run: the DROP switch
# svc_rtc_partial_wait_ab, one settled-iops.sh run of nfs4k.sh <n>, the
# switch back to Y; rtc_stats' partial and no_peek over the run.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r129}
P=/sys/module/sunrpc/parameters/svc_rtc_partial_wait_ab
S=/sys/kernel/debug/nfsd/rtc_stats
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the round's sunrpc"; exit 1; }
st() { awk -v k=$1 '$1 == k {print $2}' $S; }
for run in "$@"; do
	IFS=: read n v <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	p0=$(st fallback_partial); q0=$(st fallback_no_peek); r0=$(st read_offered)
	echo $v > $P
	./step.sh $L partial-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	echo Y > $P
	p1=$(st fallback_partial); q1=$(st fallback_no_peek); r1=$(st read_offered)
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v v=$v -v rc=$rc '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy" && !b) b = $(i + 1) } sub("%", "", b); printf "t2-r%s partial_wait %s rc %s: %.2f us of server CPU a READ\n", n, v, rc, b / 100 * 64 / m }'
	echo "t2-r$n reads $((r1 - r0)), partial waits $((p1 - p0)), no_peek to threads $((q1 - q0))"
	awk -v n=$n '$1 == "Average:" && $2 ~ /^[0-9]+$/ { b = 100 - $NF; c = $2 + 0; if (c >= 8 && c <= 55) w[++nw] = b; else r[++nr] = b } END { asort(w); asort(r); printf "t2-r%s work CPUs min %.0f median %.0f max %.0f; ring CPUs min %.0f median %.0f max %.0f\n", n, w[1], w[int(nw/2)], w[nw], r[1], r[int(nr/2)], r[nr] }' /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt
	say "after $n: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt), switch $(cat $P)"
done
