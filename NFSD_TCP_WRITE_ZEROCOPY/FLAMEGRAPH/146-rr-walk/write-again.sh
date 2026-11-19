#!/bin/bash
# write-again.sh: FLAMEGRAPH round 146, P's WRITE pass again after
# t2-r1180's RERUN (75.76 GB/s, port a 37.09 with p99 228 ms), the rule's
# second pass, then B's beside it on drives in the same state: P
# (/root/zc-ckern/LL) t2-r1183, B (the client's updates/) t2-r1184; the
# client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
say() { echo "$(date +%T) $*"; }
for n in 1183 1184; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/LL/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
pass() { say "$1: the WRITE pass t2-r$2: $(./write-check.sh $2 2>&1 | grep 'write check' | tail -1)"
	grep -E "^  write:" $L/t2-r$2/run.log | cut -c1-110; }
arm P; pass P 1183
arm B; pass B 1184
./settled.sh audit rest 2>&1 | tail -1
say "write-again all done"
