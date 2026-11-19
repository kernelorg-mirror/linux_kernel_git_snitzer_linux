#!/bin/bash
# ab-shared.sh: FLAMEGRAPH round 145, the two global lines round 144's
# perf c2c found (Mike, 19 November: "yes, do fixes 1 and 2 as round
# 145").  B: the client's updates/ stack (round 139's code, sunrpc
# 2355B960B6A9951899AF6B4, cswap131.sh stock).  P: the tip with
# 49c73b8d1c2d8 and bdf70175fc522 (/root/zc-ckern/SHR, cbuild136.sh).
# 4 KiB random READs at iodepth 96, B P B P B P (t2-r1164 to r1169);
# round 144's perf c2c on P (r1170), with P's addresses of unix_auth and
# nfs_attr_generation_counter kept beside it; then on P the WRITE pass
# (r1171) and the READ pass (r1172); the client back on its updates/
# stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1164 1165 1166 1167 1168 1169 1170 1171 1172; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/SHR/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G shr-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1164:B 1165:P 1166:B 1167:P 1168:B 1169:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
./step.sh $G c2c-1170 ../FLAMEGRAPH/144-client-c2c/c2c.sh 1170 96 5 > $G/t2-r1170.log 2>&1
say "P: perf c2c $(grep -E "t2-r1170 " $G/t2-r1170.log | tail -1 | cut -d' ' -f2-)"
$SSHC "grep -wE 'unix_auth|nfs_attr_generation_counter|nfsiod_workqueue' /proc/kallsyms" > $L/t2-r1170/syms.txt
say "P: the WRITE pass t2-r1171: $(./write-check.sh 1171 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1172: $(./read-check.sh 1172 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-shared all done"
