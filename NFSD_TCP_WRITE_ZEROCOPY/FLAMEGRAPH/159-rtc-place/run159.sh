#!/bin/bash
# run159.sh: FLAMEGRAPH round 159 (Mike, 19 November: "stop 158, just
# redo as 159").  The server's stack from the tip with both rtc fixes,
# cae2f2ea2e5b1 ("SUNRPC: Run a moved rtc connection's work item on its
# new CPU") and efe70d0679e89 ("SUNRPC: Place an rtc connection on a
# receive outside softirq too"): built (build-svcstack.sh, /dev/shm) and
# swapped into updates/, the move-only stack it replaces left in
# /dev/shm/zc-svcstack-move/stack and the one before both in
# /dev/shm/zc-stack-pre-move/.  Then settled.sh apply, the WRITE pass
# (t2-r1224), the READ pass (t2-r1225), four IOPS runs with no tracing
# (t2-r1226 to r1229) and one with conn.bt (t2-r1230), each through
# ../155-rtc-placement/placement.sh.  Waits first for round 158's last
# run, t2-r1223, to end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541; B=/dev/shm/zc-svcstack-place; PL=../FLAMEGRAPH/155-rtc-placement/placement.sh
say() { echo "$(date +%T) $*"; }
for n in $(seq 1224 1230); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
until grep -q "pk-1223: done" $G/steps.log; do sleep 5; done
say "t2-r1223 (round 158, move fix only): $(grep -E "t2-r1223 " $G/t2-r1223.log | tail -1 | cut -d' ' -f2-)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "NFS connections are up"; exit 1; }
say "tree $(git -C ../.. log --oneline -1 -- net/sunrpc include/linux/sunrpc)"
say "running: sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
./step.sh $G build-place ./build-svcstack.sh $B > /dev/null 2>&1; tail -9 $G/build-place.out | sed 's/^/    /'
grep -q "^built: " $G/build-place.out || { say "build failed"; exit 1; }
./step.sh $G swap-place env STACK=$B/stack SUNRPC_KO=none ./swap-sunrpc.sh > /dev/null 2>&1; tail -4 $G/swap-place.out | sed 's/^/    /'
say "now: sunrpc $(cat /sys/module/sunrpc/srcversion) (built $(modinfo -F srcversion $B/stack/sunrpc.ko))"
[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $B/stack/sunrpc.ko)" ] || { say "the new sunrpc is not loaded"; exit 1; }
say "$(./settled.sh apply 2>&1 | tail -1)"
say "the WRITE pass t2-r1224: $(./write-check.sh 1224 2>&1 | grep 'write check' | tail -1)"
say "the READ pass t2-r1225: $(./read-check.sh 1225 2>&1 | grep 'read check' | tail -1)"
$PL 1226 1227 1228 1229
CONN=1 $PL 1230
say "after: $(./settled.sh audit rest 2>&1 | tail -1)"
say "run159 all done"
