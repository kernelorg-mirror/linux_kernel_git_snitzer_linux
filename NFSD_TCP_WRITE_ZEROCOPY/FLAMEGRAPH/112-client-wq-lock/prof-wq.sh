#!/bin/bash
# prof-wq.sh <n>: FLAMEGRAPH round 112, the client's workqueue lock at
# 2.5 M IOPS (Mike, 18 November: "profile the client's workqueue lock at
# 2.5 M IOPS").  One settled-iops.sh run of nfs4k.sh <n> (iodepth 64,
# nconnect 12, cache_shard, both borrows held) with RUNTIME=35, and on the
# client from the load's second 8, one after another so that their costs
# do not add: 6 s of perf record -a -g at 999 Hz; 6 s of perf lock
# contention -a -b -v (by caller, with stacks); 6 s of perf lock
# contention -a -b -l (by lock).  Into $L/t2-r<n>/: cli-spin-callers.txt
# (native_queued_spin_lock_slowpath's callers), cli-self.txt,
# cli-children.txt, cli-lock-callers.txt, cli-lock-addr.txt,
# cli-wq.txt (the client's workqueues and their scopes).  The rate is
# taken under the probes.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-wq.sh <n>}
O=$L/t2-r$n; CD=/dev/shm/zc-p112-cli.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
$SSHC 'for w in /sys/devices/virtual/workqueue/*/; do printf "%s %s %s\n" "$(basename $w)" "$(cat $w/affinity_scope 2>/dev/null)" "$(cat $w/max_active 2>/dev/null)"; done' > $O/cli-wq.txt 2>&1
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf record -q -a -g -F 999 -o $CD -- sleep 6 > /dev/null 2>&1"
	$SSHC "perf lock contention -a -b -v -E 12 -- sleep 6" > $O/cli-lock-callers.txt 2>&1
	$SSHC "perf lock contention -a -b -l -E 20 -- sleep 6" > $O/cli-lock-addr.txt 2>&1
) &
RUNTIME=35 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n; rc=$?
wait
R="perf report -i $CD"
$SSHC "$R --no-children -S native_queued_spin_lock_slowpath -g caller,0.5,callee,function,percent --percent-limit 0.5 --stdio 2>/dev/null" | grep -v '^#' | grep . | head -300 > $O/cli-spin-callers.txt
$SSHC "$R --no-children -g none --sort sym --percent-limit 0.1 --stdio 2>/dev/null" | grep -v '^#' | grep . | head -150 > $O/cli-self.txt
$SSHC "$R --children -g none --sort sym --percent-limit 0.5 --stdio 2>/dev/null" | grep -v '^#' | grep . | head -150 > $O/cli-children.txt
$SSHC "rm -f $CD"
exit $rc
