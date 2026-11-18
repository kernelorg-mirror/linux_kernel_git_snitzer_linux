#!/bin/bash
# rev136.sh: FLAMEGRAPH round 136, the passes on the tip with the
# per-CPU choice and what was built on it reverted (Mike, 18 November:
# "revert each from the tip in commit order").  The client on the tip's
# stack (/root/zc-ckern/REV, cbuild136.sh): t2-r1118 WRITE, t2-r1119
# READ, t2-r1120 4 KiB IOPS at iodepth 96; then build 541's stack.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1118 1119 1120; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
say "swap: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/REV/root/ 2>&1 | tail -1")"
w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
EXP_CLI_WQ="rpciod/per_cpu=$w" ../FLAMEGRAPH/136-write-regression/prof-write.sh 1118 > /dev/null 2>&1
O=$L/t2-r1118
say "t2-r1118 WRITE: $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out)"
say "  t9 queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-9.txt 2>/dev/null | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"
EXP_CLI_WQ="rpciod/per_cpu=$w" ./read-check.sh 1119 > $G/t2-r1119.log 2>&1
say "t2-r1119 READ: $(grep -E 'read check' $G/t2-r1119.log | tail -1 | cut -d' ' -f2-)"
EXP_CLI_WQ="rpciod/per_cpu=$w" ./step.sh $G rev-1120 ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 1120:96 > $G/t2-r1120.log 2>&1
line=$(grep -E "t2-r1120 " $G/t2-r1120.log | tail -1)
say "$(echo "$line" | cut -d' ' -f2-)"
echo "$line" | awk '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r1120 IOPS: server %.2f us, client %.2f us of CPU a READ\n", s / 100 * 64 / m, c / 100 * 48 / m }'
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "rev136 all done"
