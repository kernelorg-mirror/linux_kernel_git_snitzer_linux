#!/bin/bash
# ab-xid.sh: FLAMEGRAPH round 164, the client's requests awaiting a reply
# found by their XID's low bits (Mike, 20 November: "yes, write it with
# the rb-tree fallback").  B: the client's updates/ stack (round
# 163's, f5c0543b9f377 + 4352483fa4691, sunrpc 72B6522F5FD84CCD583F45D,
# cswap131.sh stock).  X: the same with 0e9aa447e911e ("SUNRPC: Find a
# request awaiting its reply by its XID's low bits"), /root/zc-ckern/XT,
# sunrpc F55891CC4033B4C039DED19.  4 KiB random READs at iodepth 96, B X
# B X B X (t2-r1245 to r1250); then on X the WRITE pass (r1251) and the
# READ pass (r1252); the client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in $(seq 1245 1252); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; X) r=/root/zc-ckern/XT/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1"); client sunrpc $($SSHC cat /sys/module/sunrpc/srcversion)"; }
io() {
	./step.sh $G xid-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1245:B 1246:X 1247:B 1248:X 1249:B 1250:X; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
say "X: the WRITE pass t2-r1251: $(./write-check.sh 1251 2>&1 | grep 'write check' | tail -1)"
say "X: the READ pass t2-r1252: $(./read-check.sh 1252 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-xid all done"
