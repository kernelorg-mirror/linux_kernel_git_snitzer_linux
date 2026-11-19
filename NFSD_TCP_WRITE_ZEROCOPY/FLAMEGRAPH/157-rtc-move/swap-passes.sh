#!/bin/bash
# swap-passes.sh: FLAMEGRAPH round 157, the server's stack from the tip
# with cae2f2ea2e5b1 ("SUNRPC: Run a moved rtc connection's work item on
# its new CPU"; Mike, 19 November: "yes, build and swap in the fixed
# stack, then run the passes").  The struct svc_rtc_conn change versions
# sunrpc's exports anew, so the whole stack is built (build-svcstack.sh,
# in /dev/shm) and swapped into updates/ (swap-sunrpc.sh STACK=...).  The
# stack it replaces (updates/, 18 November) is kept in
# /dev/shm/zc-stack-pre-move/ first; to go back:
# STACK=/dev/shm/zc-stack-pre-move SUNRPC_KO=none ./swap-sunrpc.sh.
# Then settled.sh apply, the WRITE pass (t2-r1220) and the READ pass
# (t2-r1221).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541; B=/dev/shm/zc-svcstack-move; K=/dev/shm/zc-stack-pre-move
U=/lib/modules/$(uname -r)/updates
say() { echo "$(date +%T) $*"; }
for n in 1220 1221; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "NFS connections are up"; exit 1; }
say "tree $(git -C ../.. log --oneline -1 -- net/sunrpc include/linux/sunrpc)"
say "running: sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
rm -rf $K && mkdir -p $K && (cd $U && cp $(cat .svcstack) $K/) && say "kept: $(ls $K | wc -l) modules in $K"
./step.sh $G build-move ./build-svcstack.sh $B > /dev/null 2>&1; tail -10 $G/build-move.out | sed 's/^/    /'
grep -q "^built: " $G/build-move.out || { say "build failed"; exit 1; }
./step.sh $G swap-move env STACK=$B/stack SUNRPC_KO=none ./swap-sunrpc.sh > /dev/null 2>&1; tail -6 $G/swap-move.out | sed 's/^/    /'
say "now: sunrpc $(cat /sys/module/sunrpc/srcversion) (built $(modinfo -F srcversion $B/stack/sunrpc.ko)), nfsd $(cat /sys/module/nfsd/srcversion) (built $(modinfo -F srcversion $B/stack/nfsd.ko))"
[ "$(cat /sys/module/sunrpc/srcversion)" = "$(modinfo -F srcversion $B/stack/sunrpc.ko)" ] || { say "the new sunrpc is not loaded"; exit 1; }
say "$(./settled.sh apply 2>&1 | tail -1)"
say "the WRITE pass t2-r1220: $(./write-check.sh 1220 2>&1 | grep 'write check' | tail -1)"
say "the READ pass t2-r1221: $(./read-check.sh 1221 2>&1 | grep 'read check' | tail -1)"
say "after: $(./settled.sh audit rest 2>&1 | tail -1)"
say "swap-passes all done"
