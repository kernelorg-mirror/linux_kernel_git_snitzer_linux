#!/bin/bash
# ab-recycle.sh <n>:<pages> ...: the reply page recycle's A/B (round 104,
# 16 November).  Each pair is one read-check.sh <n> (the locked READ
# pass) with sunrpc's svc_tcp_reply_recycle at <pages> (0: off) from
# before the run, and prints beside its rate the recycle's counters over
# the run (svc_tcp_reply_recycle_stats: replies and pages sent under a
# descriptor, pages reused, let go still referenced, freed by a full
# cache or a trim, replies spliced for want of a descriptor, sends TCP
# copied) and those still live and cached at its end.  WRITE=1 runs
# write-check.sh <n> instead.  A rate under the mark goes on to the next
# pair; a run that did not finish, or a new kernel warning, stops it.
# Ends with the recycle at 0 and settled.sh apply.  Run each pair
# through step.sh.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=/var/tmp/zc-run-logs
P=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle
S=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle_stats
say() { echo "$(date +%T) $*"; }
[ -w $P ] || { say "no svc_tcp_reply_recycle: a sunrpc without the recycle"; exit 1; }
for pair in "$@"; do
	IFS=: read n pages <<< "$pair"
	O=$L/t2-r$n; mkdir -p $O
	echo $pages > $P || exit 1
	cat $S > $O/recycle.0
	if [ "${WRITE:-0}" = 1 ]; then ./write-check.sh $n; else ./read-check.sh $n; fi
	rc=$?
	cat $S > $O/recycle.1
	say "t2-r$n recycle $(cat $P): $(grep -E '^total' $O/run.log)"
	echo "    over the run: $(paste -d' ' <(tr ' ' '\n' < $O/recycle.0 | paste - -) <(tr ' ' '\n' < $O/recycle.1 | paste - -) |
		awk '$1 == "live" || $1 == "cached" {printf "%s %s, ", $1, $4; next} {printf "%s %d, ", $1, $4 - $2}')"
	grep -q '^total' $O/run.log &&
		[ "$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')" = 0 ] ||
		{ say "t2-r$n did not finish clean (exit $rc); stopping"; break; }
done
echo 0 > $P
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
