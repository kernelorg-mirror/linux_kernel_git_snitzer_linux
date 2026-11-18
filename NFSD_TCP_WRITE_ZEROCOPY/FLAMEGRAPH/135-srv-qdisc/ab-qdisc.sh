#!/bin/bash
# ab-qdisc.sh <n>:<qdisc> ...: FLAMEGRAPH round 135, the server ports'
# qdisc for 4 KiB READs (Mike, 18 November: "run the qdisc A/B next,
# quick").  The client on round 132's P stack, iodepth 96.  Each run is
# one settled-iops.sh run of nfs4k.sh <n>:96 with SRV_QDISC=<qdisc>,
# which twoport.sh puts under each transmit queue of both ports and
# takes off again after (fq_codel is the settled one).  Printed: IOPS,
# each host's CPU a READ, the qdisc the run had.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
L=${LOGS:-/var/tmp/zc-run-logs/r135}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
s=$($SSHC 'cat /sys/module/sunrpc/srcversion')
[ "$s" = C4A23C35035DD3A1E1C967B ] || { say "the client is not on P ($s)"; exit 1; }
for run in "$@"; do
	IFS=: read n q <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	if [ "$q" = fq_codel ]; then
		./step.sh $L qdisc-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:96 > $L/t2-r$n.log 2>&1
	else
		SRV_QDISC=$q ./step.sh $L qdisc-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:96 > $L/t2-r$n.log 2>&1
	fi
	rc=$?
	had=$(grep -h "qdisc .* for this run" /var/tmp/zc-run-logs/t2-r$n/run/geometry.txt 2>/dev/null | paste -sd';')
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v q=$q -v rc=$rc '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s qdisc %s rc %s: server %.2f us, client %.2f us of CPU a READ\n", n, q, rc, s / 100 * 64 / m, c / 100 * 48 / m }'
	say "t2-r$n had: ${had:-the settled fq_codel}; now $(tc qdisc show dev enp161s0f0 | awk '$2 != "mq" {print $2}' | sort -u | paste -sd,) / $(tc qdisc show dev enp164s0f0 | awk '$2 != "mq" {print $2}' | sort -u | paste -sd,)"
	say "after $n: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
done
say "r135 sequence done"
