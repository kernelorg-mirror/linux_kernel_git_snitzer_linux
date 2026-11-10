#!/bin/bash
# run371.sh: round 89, the loan's release traced.  Two two-port loan runs
# as round 88's (t2-r371, t2-r372), each with rel.bt for 10 s inside the
# window, from 5 s after the timed load starts.  The tip's sunrpc stays in
# updates/ (srcversion checked, as round 88's run367.sh).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
BT=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/89-release-trace/rel.bt
SRCV=F99F549CC7AF44FA39DAE34
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = $SRCV ] || { say "sunrpc loaded is not the tip's ($SRCV)"; exit 1; }
echo N > /sys/module/page_pool/parameters/rx_region_held_mark
for n in ${RUNS:-371 372}; do
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0; cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
	( timed_load $O/rel 5 || exit 1
	  timeout -s INT 10 bpftrace $BT > $O/rel.txt 2>&1 ) &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=rel WRITES=128 ./twoport.sh $O/rel ab > $O/rel.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/rel.log | head -3)"; wait; exit 1; }
	wait
	cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1; dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n | $(grep '^total' $O/rel.log) | $(grep -oE 'WRITEs [0-9]+' $O/rel.log | head -1) | retransmits $(grep -oE 'client retransmits in the window: [0-9]+' $O/rel.log | grep -oE '[0-9]+$') | $(grep -E 'region pages in use at most' $O/rel.log | tail -1) | $(grep -c '^Attached' $O/rel.txt) trace | new kernel warnings $hung"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
