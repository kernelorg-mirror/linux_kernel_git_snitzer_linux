#!/bin/bash
# ab-recvok.sh: FLAMEGRAPH round 141, round 140 with its own mark, so
# that NOWAIT READs complete in the receive worker too (Mike, 19
# November: "yes, add the flag and rerun the IOPS A/B").  B: the
# client's updates/ stack (the tip with round 136's reverts, sunrpc
# 2355B960B6A9951899AF6B4, cswap131.sh stock).  P: the tip with
# f58af467a2ad5 (/root/zc-ckern/DEF2, cbuild136.sh).  4 KiB random
# READs at iodepth 96, B P B P B P (t2-r1145 to r1150); round 139's
# handoff trace on P (r1151); then on P the WRITE pass (r1152) and the
# READ pass (r1153); the client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1145 1146 1147 1148 1149 1150 1151 1152 1153; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/DEF2/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G rok-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1145:B 1146:P 1147:B 1148:P 1149:B 1150:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
./step.sh $G handoff-1151 ../FLAMEGRAPH/139-handoff/handoff.sh 1151 96 0.3 > $G/t2-r1151.log 2>&1
say "P: the handoff trace $(grep -E "t2-r1151 " $G/t2-r1151.log | tail -1 | cut -d' ' -f2-)"
sed 's/^/    /' $L/t2-r1151/handoff.txt
say "P: the WRITE pass t2-r1152: $(./write-check.sh 1152 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1153: $(./read-check.sh 1153 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-recvok all done"
