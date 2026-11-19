#!/bin/bash
# p-read-prof.sh: FLAMEGRAPH round 138 (Mike, 19 November: "yes, rerun
# the READ pass on P and profile P").  The client on P
# (/root/zc-ckern/IOS): the READ pass t2-r1135, then round 137's
# two-host profile at iodepth 96, t2-r1136 (prof-both137.sh); the
# client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1135 1136; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
say "arm P: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/IOS/root/ 2>&1 | tail -1")"
say "P: the READ pass t2-r1135: $(./read-check.sh 1135 2>&1 | grep 'read check' | tail -1)"
./step.sh $G prof-1136 ../FLAMEGRAPH/137-profile-revert/prof-both137.sh 1136 96 > $G/t2-r1136.log 2>&1
line=$(grep -E "t2-r1136 " $G/t2-r1136.log | tail -1)
say "$(echo "$line" | cut -d' ' -f2-)"
echo "$line" | awk '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r1136 P: server %.2f us, client %.2f us of CPU a READ\n", s / 100 * 64 / m, c / 100 * 48 / m }'
say "arm B: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "p-read-prof all done"
