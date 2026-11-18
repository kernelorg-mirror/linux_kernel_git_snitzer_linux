#!/bin/bash
# ab-fh.sh <n>:<slots> ...: FLAMEGRAPH round 114, the file handle borrow
# (Mike, 18 November: "yes, implement the file handle borrow"), A/B of
# fh_borrow_slots, the decoded handles each thread keeps (0 disables).
# For each run: nfsd's fh_borrow_slots set to <slots>, then one
# settled-iops.sh run of nfs4k.sh <n> at the settled IOPS shape (iodepth
# 64, nconnect 12, both borrows held, the client at scope cpu).
# settled-iops.sh restarts nfs-server on its way in, so every nfsd thread
# and rtc pool rqst of the run starts with <slots>; the parameter is read
# only as a slot is made.  The borrow counters (debugfs nfsd/export_borrow,
# the fh_ ones the handle borrow's) are read before and after each run and
# their change printed beside it.  The parameter is left at 16 at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
P=/sys/module/nfsd/parameters/fh_borrow_slots
B=/sys/kernel/debug/nfsd/export_borrow
L=${L:-/var/tmp/zc-run-logs/r114}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the file handle borrow's nfsd"; exit 1; }
[ -r $B ] || mount -t debugfs none /sys/kernel/debug
delta() {	# delta "<before>" "<after>": each counter's change
	awk -v a="$1" -v b="$2" 'BEGIN {
		n = split(a, x, " "); split(b, y, " ")
		for (i = 1; i < n; i += 2) printf "%s %d ", x[i], y[i + 1] - x[i + 1]
		print "" }'
}
for run in "$@"; do
	IFS=: read n slots <<< "$run"
	[ -n "$n" ] && [ -n "$slots" ] || { say "ab-fh.sh <n>:<slots> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	echo $slots > $P || exit 1
	say "t2-r$n: fh_borrow_slots $(cat $P)"
	b0=$(cat $B)
	./step.sh $L fh-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	b1=$(cat $B)
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n fh slots $slots rc $rc borrow: $(delta "$b0" "$b1")"
done
echo 16 > $P
