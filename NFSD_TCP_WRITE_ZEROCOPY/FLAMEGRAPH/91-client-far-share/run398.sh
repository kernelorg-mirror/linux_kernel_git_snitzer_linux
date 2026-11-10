#!/bin/bash
# run398.sh: round 91, this tree's client series factored out onto the
# client's own tag (branch kernel-7.3/hs-469.client-sunrpc), against the
# tag with WQ_SYSFS alone (kernel-7.3/hs-469.sunrpc-wq-sysfs).  Every
# module on sunrpc built on the client by cbuild.sh for each, loaded from
# its module root by cswap.sh.  Arms: A, the WQ_SYSFS build as stock runs
# it; S, the same with affinity_strict 1 on rpciod and xprtiod (run392's
# B); F, the series (rpciod per-CPU, a transport per CPU homed on its
# NIC's node, the first leg in the caller; xprt_numa_map Y), as it is.
# Runs as round 90's, A S F F S A A S F, from t2-r398; stock back at the
# end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/91-client-far-share
CB=/root/zc-csunrpc
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
scp -q $SSH_OPTS $HERE/cswap.sh root@$CLIMGMT:$CB/cswap.sh || exit 1
stock() { $SSHC "bash $CB/cswap.sh stock" < /dev/null > $L/t2-r398-cswap-stock.txt 2>&1; say "client back to stock: $(tail -1 $L/t2-r398-cswap-stock.txt)"; }
n=${N0:-398}
for v in ${ARMS:-A S F F S A A S F}; do
	case $v in F) KR=/root/zc-ckern/F/root/ ;; *) KR=/root/zc-ckern/A/root/ ;; esac
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stock; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stock; exit 1; }
	$SSHC "bash $CB/cswap.sh $KR" < /dev/null > $O/cswap.txt 2>&1 || { say "$v: swap failed: $(tail -3 $O/cswap.txt)"; stock; exit 1; }
	want=$($SSHC "modinfo -F srcversion $KR/lib/modules/\$(uname -r)/updates/sunrpc.ko" < /dev/null); got=$($SSHC "cat /sys/module/sunrpc/srcversion" < /dev/null)
	[ "$want" = "$got" ] || { say "$v: the client runs sunrpc $got, not $want"; stock; exit 1; }
	if [ $v = S ]; then
		$SSHC "for w in rpciod xprtiod; do echo 1 > /sys/devices/virtual/workqueue/\$w/affinity_strict; done" < /dev/null
	fi
	$SSHC 'for w in rpciod xprtiod; do d=/sys/devices/virtual/workqueue/$w; [ -e $d/affinity_strict ] && echo "$w scope $(cat $d/affinity_scope) strict $(cat $d/affinity_strict)" || { [ -e $d/per_cpu ] && echo "$w per_cpu $(cat $d/per_cpu)"; } || echo "$w not in sysfs"; done; echo "xprt_numa_map $(cat /sys/module/sunrpc/parameters/xprt_numa_map 2>/dev/null || echo -)"' < /dev/null > $O/cli-wq.txt 2>&1
	[ $v != S ] || grep -q 'rpciod scope .* strict 1' $O/cli-wq.txt || { say "S: rpciod not strict: $(cat $O/cli-wq.txt)"; stock; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stock; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stock; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=wq-$v WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; stock; exit 1; }
	dmesg > $O/dmesg.1
	$SSHC 'for w in rpciod xprtiod; do d=/sys/devices/virtual/workqueue/$w; [ -d $d ] && echo "$w scope $(cat $d/affinity_scope) strict $(cat $d/affinity_strict)"; done; dmesg | tail -5' < /dev/null > $O/cli-wq.1 2>&1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $v | $(paste -sd' ' $O/cli-wq.txt) | client idle $(awk '$2=="all" || $3=="all" {print $NF}' $O/pmu/cli-mpstat.txt | tail -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | far $(grep "far node" $O/pmu.log | sed 's/client NIC of //; s/ of its packets sent from the far node.s CPUs//' | paste -sd' ') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stock; exit 1; }
	n=$((n + 1))
done
stock
