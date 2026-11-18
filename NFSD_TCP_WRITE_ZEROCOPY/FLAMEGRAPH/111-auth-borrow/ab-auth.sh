#!/bin/bash
# ab-auth.sh <n>:<Y|N> ...: FLAMEGRAPH round 111, the per-rqst AUTH_SYS
# borrow (Mike, 18 November: "do the auth cache borrow and profile the
# client too").  For each run: sunrpc's svcauth_borrow set to Y or N
# (read on every call, so no restart), then one settled-iops.sh run of
# nfs4k.sh <n> at the settled IOPS shape (iodepth 64, nconnect 12,
# cache_shard).  The borrow's counters (sunrpc.svcauth_borrow_stats)
# and the export borrow's (debugfs nfsd/export_borrow) are read before
# and after each run.  svcauth_borrow is left at Y, the default.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
P=/sys/module/sunrpc/parameters/svcauth_borrow
S=/sys/module/sunrpc/parameters/svcauth_borrow_stats
E=/sys/kernel/debug/nfsd/export_borrow
L=${L:-/var/tmp/zc-run-logs/r111}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the auth-borrow sunrpc"; exit 1; }
[ -r $E ] || mount -t debugfs none /sys/kernel/debug
delta() {	# delta "<before>" "<after>": each counter's change
	awk -v a="$1" -v b="$2" 'BEGIN {
		n = split(a, x, " "); split(b, y, " ")
		for (i = 1; i < n; i += 2) printf "%s %d ", x[i], y[i + 1] - x[i + 1]
		print "" }'
}
for run in "$@"; do
	IFS=: read n on <<< "$run"
	[ -n "$n" ] && [ -n "$on" ] || { say "ab-auth.sh <n>:<Y|N> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	echo $on > $P || exit 1
	s0=$(cat $S); e0=$(cat $E)
	./step.sh $L auth-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	s1=$(cat $S); e1=$(cat $E)
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n svcauth_borrow $on rc $rc auth: $(delta "$s0" "$s1")| export: $(delta "$e0" "$e1")"
done
echo Y > $P
