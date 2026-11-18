#!/bin/bash
# ab-iostats.sh <n>:<0|1> ...: FLAMEGRAPH round 134, block accounting
# off on the eight data drives (Mike, 18 November: "run the iostats=0
# A/B, quick").  The client on round 132's P stack, iodepth 96.  For
# each run /sys/block/nvme[0-7]n1/queue/iostats is set (never nvme8,
# the root disk), one settled-iops.sh run of nfs4k.sh <n>:96, and
# iostats back to 1.  Printed: IOPS, each host's CPU a READ.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
L=${LOGS:-/var/tmp/zc-run-logs/r134}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
set_io() { for i in 0 1 2 3 4 5 6 7; do echo $1 > /sys/block/nvme${i}n1/queue/iostats; done; }
s=$($SSHC 'cat /sys/module/sunrpc/srcversion')
[ "$s" = C4A23C35035DD3A1E1C967B ] || { say "the client is not on P ($s)"; exit 1; }
for run in "$@"; do
	IFS=: read n v <<< "$run"
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	set_io $v
	got=$(cat /sys/block/nvme[0-7]n1/queue/iostats | sort -u | paste -sd,)
	./step.sh $L iostats-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:96 > $L/t2-r$n.log 2>&1
	rc=$?
	set_io 1
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v v=$got -v rc=$rc '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s iostats %s rc %s: server %.2f us, client %.2f us of CPU a READ\n", n, v, rc, s / 100 * 64 / m, c / 100 * 48 / m }'
	say "after $n: iostats $(cat /sys/block/nvme[0-7]n1/queue/iostats | sort -u | paste -sd,); $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
done
say "r134 sequence done"
