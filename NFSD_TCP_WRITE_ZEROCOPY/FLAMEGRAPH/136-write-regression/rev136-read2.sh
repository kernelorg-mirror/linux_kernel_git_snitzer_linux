#!/bin/bash
# rev136-read2.sh: FLAMEGRAPH round 136, a second READ pass on the tip
# with the per-CPU choice reverted (Mike, 18 November: "yes, run a second
# READ pass after IOPS"): t2-r1119 read 77.84 GB/s, port b's drives 9.3
# GB/s each at 100% busy against 10.1 to 10.3 at t2-r1113.  t2-r1121 on
# /root/zc-ckern/REV, then build 541's stack.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
[ -e $L/t2-r1121 ] && { say "t2-r1121 exists"; exit 1; }
say "swap: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/REV/root/ 2>&1 | tail -1")"
w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
EXP_CLI_WQ="rpciod/per_cpu=$w" ./read-check.sh 1121 > $G/t2-r1121.log 2>&1
say "t2-r1121 READ: $(grep -E 'read check' $G/t2-r1121.log | tail -1 | cut -d' ' -f2-)"
say "  $(grep -E '^  read:' $L/t2-r1121/run.log | awk '{printf "%s ", $2}') GB/s a port; $(sed -n 1,2p $L/t2-r1121/run/drives.txt | grep -oE 'reading [0-9.]+ to [0-9.]+' | paste -sd' ')"
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "rev136-read2 all done"
