#!/bin/bash
# ab-xid.sh: FLAMEGRAPH round 143, a transport's requests awaiting
# their replies hashed by xid instead of kept in an rb-tree (Mike, 19
# November: "yes, write the xid table and run the IOPS A/B").  B: the
# client's updates/ stack (round 139's code, sunrpc
# 2355B960B6A9951899AF6B4, cswap131.sh stock).  P: the tip with
# 58b51d007a76e (/root/zc-ckern/XID, cbuild136.sh).  4 KiB random READs
# at iodepth 96, B P B P B P (t2-r1155 to r1160); then on P the WRITE
# pass (r1161) and the READ pass (r1162); the client back on its
# updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1155 1156 1157 1158 1159 1160 1161 1162; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/XID/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G xid-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1155:B 1156:P 1157:B 1158:P 1159:B 1160:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
say "P: the WRITE pass t2-r1161: $(./write-check.sh 1161 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1162: $(./read-check.sh 1162 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-xid all done"
