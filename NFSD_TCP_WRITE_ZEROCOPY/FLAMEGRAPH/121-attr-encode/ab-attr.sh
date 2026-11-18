#!/bin/bash
# ab-attr.sh <n>:<new|old> ...: FLAMEGRAPH round 121, the NFSv3 post-op
# attribute encode made cheaper (Mike, 18 November: "implement the
# post-op attribute fast path and A/B it").  For each run: nfsd's
# attr_encode_ab N (new) or Y (old, as before the round), one
# settled-iops.sh run of nfs4k.sh <n>, attr_encode_ab back to N.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r121}
P=/sys/module/nfsd/parameters/attr_encode_ab
mkdir -p $L
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no $P: not the round's nfsd"; exit 1; }
for run in "$@"; do
	IFS=: read n spec <<< "$run"
	case $spec in new) v=N ;; old) v=Y ;; *) say "ab-attr.sh <n>:<new|old> ..."; exit 1 ;; esac
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	echo $v > $P
	./step.sh $L attr-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	got=$(cat $P); echo N > $P
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n attr $spec (attr_encode_ab $got in the run) rc $rc | after: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
done
