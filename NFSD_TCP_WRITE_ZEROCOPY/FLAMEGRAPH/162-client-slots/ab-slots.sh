#!/bin/bash
# ab-slots.sh <n>:<B|S> ...: FLAMEGRAPH round 162, the client's
# preallocated RPC slots for 4 KiB READ IOPS (Mike, 19 November: "yes,
# run the slot-table A/B").  B: sunrpc.tcp_slot_table_entries 2, the
# default, so with some 128 READs outstanding a transport nearly every
# slot is kzalloc'd and kfree'd (round 161).  S: 128, so they come from
# the transport's free list.  The sysctl is read when a transport is
# made, and twoport.sh mounts afresh each run.  Each run one nfs4k.sh at
# iodepth 96 under settled-iops.sh; the client back at 2 after each.
# Only the IOPS runs change: the passes run at the default.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
S=/proc/sys/sunrpc/tcp_slot_table_entries
for run in "$@"; do
	IFS=: read n arm <<< "$run"
	case $arm in B) v=2 ;; S) v=128 ;; *) echo "ab-slots.sh <n>:<B|S> ..."; exit 1 ;; esac
	$SSHC "echo $v > $S"
	echo "$(date +%T) t2-r$n arm $arm: client tcp_slot_table_entries $($SSHC cat $S)"
	line=$(./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:96 2>&1 | grep -E "t2-r$n 4k" | tail -1)
	$SSHC "echo 2 > $S"
	echo "$line" | cut -c1-260
	echo "$line" | awk -v n=$n '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s: server %.2f us, client %.2f us of CPU a READ\n", n, s / 100 * 64 / m, c / 100 * 48 / m }'
done
echo "$(date +%T) ab-slots done; client tcp_slot_table_entries $($SSHC cat $S)"
