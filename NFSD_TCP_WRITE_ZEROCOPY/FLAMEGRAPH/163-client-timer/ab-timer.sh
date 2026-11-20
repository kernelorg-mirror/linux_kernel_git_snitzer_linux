#!/bin/bash
# ab-timer.sh: FLAMEGRAPH round 163, the client's transport idle timer
# stopped only when it is pending (Mike, 19 November: "yes, run the
# slot-table A/B, then do 2").  B: the client's updates/ stack (round
# 149's, f5c0543b9f377, sunrpc 1EFC577AC45E5EEF6EC1592, cswap131.sh
# stock).  T: f5c0543b9f377 with 4352483fa4691 ("SUNRPC: Stop a
# transport's idle timer only when it is pending"), /root/zc-ckern/TD,
# sunrpc 72B6522F5FD84CCD583F45D.  4 KiB random READs at iodepth 96, B T
# B T B T (t2-r1237 to r1242); then on T the WRITE pass (r1243) and the
# READ pass (r1244); the client back on its updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in $(seq 1237 1244); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; T) r=/root/zc-ckern/TD/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1"); client sunrpc $($SSHC cat /sys/module/sunrpc/srcversion)"; }
io() {
	./step.sh $G tmr-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1237:B 1238:T 1239:B 1240:T 1241:B 1242:T; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
say "T: the WRITE pass t2-r1243: $(./write-check.sh 1243 2>&1 | grep 'write check' | tail -1)"
say "T: the READ pass t2-r1244: $(./read-check.sh 1244 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-timer all done"
