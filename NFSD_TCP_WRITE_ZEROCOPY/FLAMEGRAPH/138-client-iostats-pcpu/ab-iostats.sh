#!/bin/bash
# ab-iostats.sh: FLAMEGRAPH round 138, the client's per-procedure RPC
# iostats per CPU (Mike, 18 November: "yes, make the RPC iostats
# per-CPU").  B: the client's updates/ stack (the tip with round 136's
# reverts, sunrpc 2355B960B6A9951899AF6B4, cswap131.sh stock).  P: the
# tip with 7e85409ea83f6 and 772a7f50696af (/root/zc-ckern/IOS,
# cbuild136.sh).  4 KiB random READs at iodepth 96, B P B P (t2-r1129 to
# r1132), with the client's mountstats READ count read after each P;
# then on P the WRITE pass (r1133) and the READ pass (r1134); the
# client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1129 1130 1131 1132 1133 1134; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/IOS/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G ios-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1129:B 1130:P 1131:B 1132:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
say "P: the WRITE pass t2-r1133: $(./write-check.sh 1133 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1134: $(./read-check.sh 1134 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-iostats all done"
