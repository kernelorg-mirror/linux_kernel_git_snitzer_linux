#!/bin/bash
# ab-array.sh: FLAMEGRAPH round 149, the round-robin choice found in the
# switch's array instead of by walking its list, on its own (Mike, 19
# November: "run the A/B with commit f5c0543b9f377").  B: the client's
# updates/ stack (round 145's, bdf70175fc522, sunrpc
# 036CCE7D414F6EAC3D6FDDD, cswap131.sh stock).  P: f5c0543b9f377
# (/root/zc-ckern/RR, round 146's first build, sunrpc
# 1EFC577AC45E5EEF6EC1592).  4 KiB random READs at iodepth 96, B P B P B
# P (t2-r1191 to r1196); then on P the WRITE pass (r1197) and the READ
# pass (r1198); the client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1191 1192 1193 1194 1195 1196 1197 1198; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/RR/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G arr-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1191:B 1192:P 1193:B 1194:P 1195:B 1196:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
say "P: the WRITE pass t2-r1197: $(./write-check.sh 1197 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1198: $(./read-check.sh 1198 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-array all done"
