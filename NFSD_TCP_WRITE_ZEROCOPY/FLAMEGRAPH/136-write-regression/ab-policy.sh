#!/bin/bash
# ab-policy.sh: FLAMEGRAPH round 136, the nconnect policy (Mike, 18
# November: "yes, revert the fix and write the policy modparam").  The
# client on the tip's stack with e42020cc5e247 (/root/zc-ckern/POL,
# cbuild136.sh), loaded with sunrpc nconnect_percpu=N (the balanced
# round-robin) and then Y (the per-CPU choice), set in
# /etc/modprobe.d/zc-nconnect.conf before each load (the parameter is
# fixed at load).  For each: the WRITE pass, the READ pass and 4 KiB
# random READs at iodepth 96:
#   N: t2-r1112 WRITE, r1113 READ, r1114 IOPS
#   Y: t2-r1115 WRITE, r1116 READ, r1117 IOPS
# The conf removed and the client back on build 541's stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1112 1113 1114 1115 1116 1117; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
arm() {
	$SSHC "echo 'options sunrpc nconnect_percpu=$1' > /etc/modprobe.d/zc-nconnect.conf"
	say "swap $1: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/POL/root/ 2>&1 | tail -1")"
	say "nconnect_percpu $($SSHC 'cat /sys/module/sunrpc/parameters/nconnect_percpu')"
	w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
}
wr() {
	EXP_CLI_WQ="rpciod/per_cpu=$w" ../FLAMEGRAPH/136-write-regression/prof-write.sh $1 > /dev/null 2>&1
	O=$L/t2-r$1
	say "t2-r$1 WRITE $2: $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out)"
	say "  t9 queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-9.txt 2>/dev/null | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"
}
rd() {
	EXP_CLI_WQ="rpciod/per_cpu=$w" ./read-check.sh $1 > $G/t2-r$1.log 2>&1
	say "t2-r$1 READ $2: $(grep -E 'read check' $G/t2-r$1.log | tail -1 | cut -d' ' -f2-)"
}
io() {
	EXP_CLI_WQ="rpciod/per_cpu=$w" ./step.sh $G pol-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s IOPS %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
arm N; wr 1112 N; rd 1113 N; io 1114 N
arm Y; wr 1115 Y; rd 1116 Y; io 1117 Y
$SSHC 'rm -f /etc/modprobe.d/zc-nconnect.conf'
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "ab-policy all done"
