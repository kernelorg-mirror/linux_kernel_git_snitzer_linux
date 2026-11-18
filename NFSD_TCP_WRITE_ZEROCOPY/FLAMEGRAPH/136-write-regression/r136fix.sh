#!/bin/bash
# r136fix.sh: FLAMEGRAPH round 136, the fix (Mike, 18 November: "yes,
# write the fix and run the passes"; the receive knob's default "will
# depend on how your fix turns out").  The client on the tip's stack
# with 866e06f9ffc48 (/root/zc-ckern/FIX, cbuild136.sh), then, each with
# sunrpc's xs_recv_on_home_cpu set at run time:
#   t2-r1106 WRITE pass, recv Y      t2-r1107 WRITE pass, recv N
#   t2-r1108 READ pass, recv Y
#   t2-r1109 4 KiB IOPS iodepth 96, recv Y   t2-r1110 the same, recv N
# The client back on build 541's stack after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
recv() { $SSHC "echo $1 > /sys/module/sunrpc/parameters/xs_recv_on_home_cpu; cat /sys/module/sunrpc/parameters/xs_recv_on_home_cpu /sys/module/sunrpc/parameters/xprt_percpu_slack | paste -sd/"; }
for n in 1106 1107 1108 1109 1110; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
say "swap: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/FIX/root/ 2>&1 | tail -1")"
w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
wr() {
	say "t2-r$1 recv/slack $(recv $2)"
	EXP_CLI_WQ="rpciod/per_cpu=$w" ../FLAMEGRAPH/136-write-regression/prof-write.sh $1 > /dev/null 2>&1
	O=$L/t2-r$1
	say "t2-r$1 WRITE recv $2: $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out)"
	say "  t9 queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-9.txt 2>/dev/null | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"
}
wr 1106 Y
wr 1107 N
say "t2-r1108 recv/slack $(recv Y)"
EXP_CLI_WQ="rpciod/per_cpu=$w" ./read-check.sh 1108 > $G/t2-r1108.log 2>&1
say "t2-r1108 READ recv Y: $(grep -E 'read check' $G/t2-r1108.log | tail -1)"
io() {
	say "t2-r$1 recv/slack $(recv $2)"
	EXP_CLI_WQ="rpciod/per_cpu=$w" ./step.sh $G fix-$1 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $1:96 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s recv %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
}
io 1109 Y
io 1110 N
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "r136fix all done"
