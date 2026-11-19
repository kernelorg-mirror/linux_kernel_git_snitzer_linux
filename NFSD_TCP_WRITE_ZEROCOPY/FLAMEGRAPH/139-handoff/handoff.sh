#!/bin/bash
# handoff.sh <n> [iodepth] [seconds]: FLAMEGRAPH round 139, where a 4 KiB
# READ's stages run on the client (Mike, 19 November: "then measure the
# cross-CPU handoff").  One settled-iops.sh run of nfs4k.sh <n>:<iodepth>
# (96) and, from the load's eighth second, <seconds> (0.3) of the
# client's sunrpc tracepoints, each record with its CPU and time:
# rpc_task_begin (the submit), xprt_transmit (the send), xprt_lookup_rqst
# (the reply matched to its request), rpc_stats_latency (the
# completion) and rpc_task_end, into the client's
# /dev/shm/zc-h139-<n>.data, kept.  Into $L/t2-r<n>/: cli-trace.txt
# (perf script) is left on the client; handoff.awk's reading is
# handoff.txt.  Only reads the client.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?handoff.sh <n> [iodepth] [seconds]}; q=${2:-96}; s=${3:-0.3}
O=$L/t2-r$n; C=/dev/shm/zc-h139-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
E=sunrpc:rpc_task_begin,sunrpc:xprt_transmit,sunrpc:xprt_lookup_rqst,sunrpc:rpc_stats_latency,sunrpc:rpc_task_end
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf record -q -a -e $E -m 8M -o $C -- sleep $s > /dev/null 2>&1"
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
$SSHC "cat > /dev/shm/handoff139.awk" < ../FLAMEGRAPH/139-handoff/handoff.awk
$SSHC "perf script -i $C -F cpu,time,event,trace 2>/dev/null | awk -f /dev/shm/handoff139.awk" > $O/handoff.txt
exit $rc
