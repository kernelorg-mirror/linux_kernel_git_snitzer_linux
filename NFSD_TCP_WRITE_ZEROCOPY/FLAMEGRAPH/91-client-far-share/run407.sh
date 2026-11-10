#!/bin/bash
# run407.sh: round 91, F with xprtiod strict (FX).  F (the client series,
# rpciod per-CPU) left 0.9% to 1.6% of the sends on the far node; xprtiod
# is still unbound and not strict there.  Arms as run398.sh's, and FX: F's
# modules with affinity_strict 1 on xprtiod (rpciod has none, per-CPU).
# A run marked T (FT, FXT) also runs round 91's client trace of the far
# sends (run388.sh's) in its window.  F T, FX T, FX, FX from t2-r407.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/91-client-far-share
CB=/root/zc-csunrpc
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
scp -q $SSH_OPTS $HERE/cswap.sh root@$CLIMGMT:$CB/cswap.sh || exit 1
ctrace() {
	$SSHC "cd /dev/shm && rm -f far-*.data st-*.txt
	f() { echo \"--filter 'name == \\\"\$1\\\"'\"; }
	E=net:net_dev_start_xmit
	eval perf stat -C 0-23 -x, -e \$E \$(f ens3np0) -e \$E \$(f ens6np0) -o st-n0.txt -- sleep $FAR_SECS &
	eval perf stat -C 24-47 -x, -e \$E \$(f ens3np0) -e \$E \$(f ens6np0) -o st-n1.txt -- sleep $FAR_SECS &
	eval perf record -q -C 24-47 -e \$E \$(f ens3np0) -c $FAR_C -g -o far-a.data -- sleep $FAR_SECS &
	eval perf record -q -C 0-23 -e \$E \$(f ens6np0) -c $FAR_C -g -o far-b.data -- sleep $FAR_SECS &
	wait
	for p in a b; do perf script -i far-\$p.data -F comm,tid,cpu,ip,sym 2>/dev/null | xz -T8 -3 > far-\$p.script.xz; done
	rm -f far-*.data" < /dev/null
}
stock() { $SSHC "bash $CB/cswap.sh stock" < /dev/null > $L/t2-r407-cswap-stock.txt 2>&1; say "client back to stock: $(tail -1 $L/t2-r407-cswap-stock.txt)"; }
FAR_C=${FAR_C:-5} FAR_SECS=${FAR_SECS:-30}
n=${N0:-407}
for vt in ${ARMS:-FT FXT FX FX}; do
	v=${vt%T}; [ "$v" != "$vt" ] && tr=1 || tr=0
	case $v in F|FX) KR=/root/zc-ckern/F/root/ ;; *) KR=/root/zc-ckern/A/root/ ;; esac
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stock; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stock; exit 1; }
	$SSHC "bash $CB/cswap.sh $KR" < /dev/null > $O/cswap.txt 2>&1 || { say "$v: swap failed: $(tail -3 $O/cswap.txt)"; stock; exit 1; }
	want=$($SSHC "modinfo -F srcversion $KR/lib/modules/\$(uname -r)/updates/sunrpc.ko" < /dev/null); got=$($SSHC "cat /sys/module/sunrpc/srcversion" < /dev/null)
	[ "$want" = "$got" ] || { say "$v: the client runs sunrpc $got, not $want"; stock; exit 1; }
	if [ $v = FX ]; then
		$SSHC "echo 1 > /sys/devices/virtual/workqueue/xprtiod/affinity_strict" < /dev/null
	fi
	if [ $v = S ]; then
		$SSHC "for w in rpciod xprtiod; do echo 1 > /sys/devices/virtual/workqueue/\$w/affinity_strict; done" < /dev/null
	fi
	$SSHC 'for w in rpciod xprtiod; do d=/sys/devices/virtual/workqueue/$w; [ -e $d/affinity_strict ] && echo "$w scope $(cat $d/affinity_scope) strict $(cat $d/affinity_strict)" || { [ -e $d/per_cpu ] && echo "$w per_cpu $(cat $d/per_cpu)"; } || echo "$w not in sysfs"; done; echo "xprt_numa_map $(cat /sys/module/sunrpc/parameters/xprt_numa_map 2>/dev/null || echo -)"' < /dev/null > $O/cli-wq.txt 2>&1
	[ $v != FX ] || grep -q 'xprtiod scope .* strict 1' $O/cli-wq.txt || { say "FX: xprtiod not strict: $(cat $O/cli-wq.txt)"; stock; exit 1; }
	[ $v != S ] || grep -q 'rpciod scope .* strict 1' $O/cli-wq.txt || { say "S: rpciod not strict: $(cat $O/cli-wq.txt)"; stock; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stock; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stock; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=wq-$vt WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 &
	tp=$!
	if [ $tr = 1 ]; then
		for i in $(seq 1 300); do [ -e $O/pmu/ss-client.txt ] && break; kill -0 $tp 2>/dev/null || break; sleep 1; done
		if [ -e $O/pmu/ss-client.txt ]; then ctrace > $O/ctrace.out 2>&1
			for f in st-n0.txt st-n1.txt far-a.script.xz far-b.script.xz; do scp -q $SSH_OPTS root@$CLIMGMT:/dev/shm/$f $O/cli-$f; done
			$SSHC "rm -f /dev/shm/far-* /dev/shm/st-n*" < /dev/null
		fi
	fi
	wait $tp || { say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; stock; exit 1; }
	dmesg > $O/dmesg.1
	$SSHC 'for w in rpciod xprtiod; do d=/sys/devices/virtual/workqueue/$w; [ -d $d ] && echo "$w scope $(cat $d/affinity_scope) strict $(cat $d/affinity_strict)"; done; dmesg | tail -5' < /dev/null > $O/cli-wq.1 2>&1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $vt | $(paste -sd' ' $O/cli-wq.txt) | client idle $(awk '$2=="all" || $3=="all" {print $NF}' $O/pmu/cli-mpstat.txt | tail -1) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | far $(grep "far node" $O/pmu.log | sed 's/client NIC of //; s/ of its packets sent from the far node.s CPUs//' | paste -sd' ') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stock; exit 1; }
	n=$((n + 1))
done
stock
