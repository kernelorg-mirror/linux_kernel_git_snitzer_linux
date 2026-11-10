#!/bin/bash
# run388.sh: round 91, who on the client sends from the far node.  Two
# two-port loan runs as round 90's (stack 96, settled), t2-r388 and
# t2-r389, no perf on the server.  Once the window opens (twoport has
# written ss-client.txt) the client counts every skb handed to each E1
# port's driver (net:net_dev_start_xmit, filtered on the device) on each
# node's CPUs, and records one in $C of those sent from the far node's
# CPUs with kernel call chains: port a (ens3np0) is near CPUs 0-23, port
# b (ens6np0) near 24-47.  The samples come back as far-*.script.xz.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
C=${C:-5} D=${D:-30}
ctrace() {
	$SSHC "cd /dev/shm && rm -f far-*.data st-*.txt
	f() { echo \"--filter 'name == \\\"\$1\\\"'\"; }
	E=net:net_dev_start_xmit
	eval perf stat -C 0-23 -x, -e \$E \$(f ens3np0) -e \$E \$(f ens6np0) -o st-n0.txt -- sleep $D &
	eval perf stat -C 24-47 -x, -e \$E \$(f ens3np0) -e \$E \$(f ens6np0) -o st-n1.txt -- sleep $D &
	eval perf record -q -C 24-47 -e \$E \$(f ens3np0) -c $C -g -o far-a.data -- sleep $D &
	eval perf record -q -C 0-23 -e \$E \$(f ens6np0) -c $C -g -o far-b.data -- sleep $D &
	wait
	for p in a b; do perf script -i far-\$p.data -F comm,tid,cpu,ip,sym 2>/dev/null | xz -T8 -3 > far-\$p.script.xz; done
	rm -f far-*.data" < /dev/null
}
for n in ${RUNS:-388 389}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	echo N > /sys/module/page_pool/parameters/rx_region_held_mark
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=far WRITES=128 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 &
	tp=$!
	for i in $(seq 1 300); do [ -e $O/pmu/ss-client.txt ] && break; kill -0 $tp 2>/dev/null || break; sleep 1; done
	if [ -e $O/pmu/ss-client.txt ]; then ctrace > $O/ctrace.out 2>&1
		for f in st-n0.txt st-n1.txt far-a.script.xz far-b.script.xz; do scp -q $SSH_OPTS root@$CLIMGMT:/dev/shm/$f $O/cli-$f; done
		$SSHC "rm -f /dev/shm/far-* /dev/shm/st-n*" < /dev/null
	fi
	wait $tp || { say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; exit 1; }
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n | $(grep '^total' $O/pmu.log) | $(grep -oE 'WRITEs [0-9]+' $O/pmu.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/pmu.log | grep -oE '[0-9]+$') | $(grep "far node" $O/pmu.log | sed 's/client NIC of //; s/ of its packets sent from the far node.s CPUs//' | paste -sd' ') | client samples $(xz -dc $O/cli-far-a.script.xz 2>/dev/null | grep -c '^[^[:space:]]') $(xz -dc $O/cli-far-b.script.xz 2>/dev/null | grep -c '^[^[:space:]]') | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
