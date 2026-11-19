#!/bin/bash
# ab-complete.sh: FLAMEGRAPH round 140, a reply completed in the
# receive worker that matched it (Mike, 19 November: "yes, complete the
# READ in the receive worker").  B: the client's updates/ stack (the tip
# with round 136's reverts, sunrpc 2355B960B6A9951899AF6B4, cswap131.sh
# stock).  P: the tip with 83b22f44df786 (/root/zc-ckern/DEF,
# cbuild136.sh).  4 KiB random READs at iodepth 96, B P B P (t2-r1138 to
# r1141); round 139's handoff trace on P (r1142); then on P the WRITE
# pass (r1143) and the READ pass (r1144); the client back on its
# updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1138 1139 1140 1141 1142 1143 1144; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/DEF/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G def-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1138:B 1139:P 1140:B 1141:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
./step.sh $G handoff-1142 ../FLAMEGRAPH/139-handoff/handoff.sh 1142 96 0.3 > $G/t2-r1142.log 2>&1
say "P: the handoff trace $(grep -E "t2-r1142 " $G/t2-r1142.log | tail -1 | cut -d' ' -f2-)"
sed 's/^/    /' $L/t2-r1142/handoff.txt
say "P: the WRITE pass t2-r1143: $(./write-check.sh 1143 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1144: $(./read-check.sh 1144 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-complete all done"
