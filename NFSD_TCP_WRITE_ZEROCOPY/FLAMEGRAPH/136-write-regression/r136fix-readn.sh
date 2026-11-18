#!/bin/bash
# r136fix-readn.sh: FLAMEGRAPH round 136, the fix's READ pass with the
# receive knob at N (Mike, 18 November: "why does READ pass not have
# Receive knob N?"; a READ's reply is where the client's receive works
# hardest).  t2-r1111 on the fix's stack (/root/zc-ckern/FIX), then the
# client back on build 541's stack.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
[ -e $L/t2-r1111 ] && { say "t2-r1111 exists"; exit 1; }
say "swap: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/FIX/root/ 2>&1 | tail -1")"
w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
say "t2-r1111 recv/slack $($SSHC "echo N > /sys/module/sunrpc/parameters/xs_recv_on_home_cpu; cat /sys/module/sunrpc/parameters/xs_recv_on_home_cpu /sys/module/sunrpc/parameters/xprt_percpu_slack | paste -sd/")"
EXP_CLI_WQ="rpciod/per_cpu=$w" ./read-check.sh 1111 > $G/t2-r1111.log 2>&1
say "t2-r1111 READ recv N: $(grep -E 'read check' $G/t2-r1111.log | tail -1)"
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "r136fix-readn all done"
