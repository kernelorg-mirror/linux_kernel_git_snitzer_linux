#!/bin/bash
# ab-pcpu.sh <n>:<B|P> ...: FLAMEGRAPH round 132, the client's call
# counts per CPU (Mike, 18 November: "yes, do step 2").  No switch can
# change struct rpc_stat's layout, so the client swaps NFS stacks: B is
# /root/zc-ckern/B (the tip before the change: round 131's counting arm
# without its switch), P is /root/zc-ckern/T (the tip, with sunrpc and
# nfs counting per CPU), each built by cbuild132.sh.  For each run the
# client is put on that arm's stack (cswap131.sh, client side only),
# then one settled-iops.sh run of nfs4k.sh <n>.  Printed: IOPS, each
# host's CPU a READ, the client's spread.  The client is left on the
# last arm's stack: cswap131.sh stock puts build 541's back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
L=${LOGS:-/var/tmp/zc-run-logs/r132}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
for run in "$@"; do
	IFS=: read n a <<< "$run"
	case $a in B) d=B ;; P) d=T ;; *) say "arm $a?"; exit 1 ;; esac
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	sw=$($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/$d/root/ 2>&1 | tail -1; echo rc=\$?")
	say "t2-r$n arm $a: $sw"
	want=$($SSHC "modinfo -F srcversion /root/zc-ckern/$d/root/lib/modules/\$(uname -r)/updates/sunrpc.ko; cat /sys/module/sunrpc/srcversion" | sort -u | wc -l)
	[ "$want" = 1 ] || { say "the client is not on arm $a's sunrpc"; exit 1; }
	./step.sh $L pcpu-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	line=$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v a=$a -v rc=$rc '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s rc %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, rc, s / 100 * 64 / m, c / 100 * 48 / m }'
	awk -v n=$n '$1 == "Average:" && $2 ~ /^[0-9]+$/ { b[++k] = 100 - $NF - $6 } END { asort(b); printf "t2-r%s client CPUs min %.0f median %.0f max %.0f\n", n, b[1], b[int(k/2)], b[k] }' /var/tmp/zc-run-logs/t2-r$n/run/cli-mpstat.txt
	say "t2-r$n client: $($SSHC 'head -2 /proc/net/rpc/nfs | tail -1; grep ^proc3 /proc/net/rpc/nfs | cut -c1-60')"
	say "after $n: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
done
say "r132 sequence done"
