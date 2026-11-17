#!/bin/bash
# ab-borrow.sh <n>:<slots> ...: FLAMEGRAPH round 110, the per-thread
# export borrow with several entries (Mike, 17 November: "build the
# borrow fix and A/B it at 0 vs 16").  For each run: nfsd's
# export_borrow_slots set to <slots> (0 disables the borrow), then one
# settled-iops.sh run of nfs4k.sh <n> at the settled IOPS shape
# (iodepth 64, nconnect 12, cache_shard).  settled-iops.sh restarts
# nfs-server on its way in, so every nfsd thread and rtc pool rqst of the
# run starts with <slots>; the parameter is read only as a slot is made.
# The borrow counters (debugfs nfsd/export_borrow) are read before and
# after each run and their change printed beside it.  The parameter is
# left at 16, the default, at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
P=/sys/module/nfsd/parameters/export_borrow_slots
B=/sys/kernel/debug/nfsd/export_borrow
L=${L:-/var/tmp/zc-run-logs/r110}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the borrow-fix nfsd"; exit 1; }
[ -r $B ] || mount -t debugfs none /sys/kernel/debug
delta() {	# delta "<before>" "<after>": each counter's change
	awk -v a="$1" -v b="$2" 'BEGIN {
		n = split(a, x, " "); split(b, y, " ")
		for (i = 1; i < n; i += 2) printf "%s %d ", x[i], y[i + 1] - x[i + 1]
		print "" }'
}
for run in "$@"; do
	IFS=: read n slots <<< "$run"
	[ -n "$n" ] && [ -n "$slots" ] || { say "ab-borrow.sh <n>:<slots> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	echo $slots > $P || exit 1
	say "t2-r$n: export_borrow_slots $(cat $P)"
	b0=$(cat $B)
	./step.sh $L borrow-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	b1=$(cat $B)
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n slots $slots rc $rc borrow: $(delta "$b0" "$b1")"
done
echo 16 > $P
