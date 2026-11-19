#!/bin/bash
# ab-rr.sh: FLAMEGRAPH round 146, the round-robin choice without the
# walk of the switch's list (Mike, 19 November: "yes, do the
# round-robin walk as round 146").  B: the client's updates/ stack
# (round 145's, bdf70175fc522, sunrpc 036CCE7D414F6EAC3D6FDDD,
# cswap131.sh stock).  P: the tip with f5c0543b9f377 (/root/zc-ckern/RR,
# cbuild136.sh).  4 KiB random READs at iodepth 96, B P B P B P
# (t2-r1173 to r1178); round 144's perf c2c on P (r1179); then on P the
# WRITE pass (r1180) and the READ pass (r1181); the client back on its
# updates/ stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1173 1174 1175 1176 1177 1178 1179 1180 1181; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma'
arm() { case $1 in B) r=stock ;; P) r=/root/zc-ckern/RR/root/ ;; esac
	say "arm $1: $($SSHC "NFSM='$NFSM' /root/zc-csunrpc/cswap131.sh $r 2>&1 | tail -1")"; }
io() {
	./step.sh $G rr-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
for x in 1173:B 1174:P 1175:B 1176:P 1177:B 1178:P; do
	n=${x%:*}; a=${x#*:}
	arm $a; io $n $a
done
./step.sh $G c2c-1179 ../FLAMEGRAPH/144-client-c2c/c2c.sh 1179 96 5 > $G/t2-r1179.log 2>&1
say "P: perf c2c $(grep -E "t2-r1179 " $G/t2-r1179.log | tail -1 | cut -d' ' -f2-)"
$SSHC "grep -wE 'xprt_iter_next_entry_roundrobin|xprt_switch_find_next_entry' /proc/kallsyms" > $L/t2-r1179/syms.txt
say "P: the WRITE pass t2-r1180: $(./write-check.sh 1180 2>&1 | grep 'write check' | tail -1)"
say "P: the READ pass t2-r1181: $(./read-check.sh 1181 2>&1 | grep 'read check' | tail -1)"
arm B
./settled.sh audit rest 2>&1 | tail -1
say "ab-rr all done"
