#!/bin/bash
# ab-reads.sh <n>:<rr> ...: READ confinement A/B on the kernel from the
# tip with David Flynn's 15 READ-confinement patches (round 104, 16
# November).  Each pair is one read-check.sh <n> (the locked READ pass:
# 1 MiB reads at rsize 256 KiB, WARM=10 RUNTIME=40, trim and 120 s rest)
# with sunrpc's svc_tcp_rx_region_reads at <rr> (N or Y) from before the
# mounts.  page_pool.rx_region_held_mark is Y in every run, so
# summary.txt's region peak is the kernel's exact max_held.  Around each
# run it saves sunrpc's budget counters (the connection's receive budget
# a confined READ draws on, and the read pool's) and the region's
# debugfs file, and prints the READ counters' change: admitted,
# parked, unconfined, short and share_stalls.  Ends with region_reads N
# and held_mark N and settled.sh apply.  Run each pair through step.sh.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=/var/tmp/zc-run-logs
RR=/sys/module/sunrpc/parameters/svc_tcp_rx_region_reads
HM=/sys/module/page_pool/parameters/rx_region_held_mark
say() { echo "$(date +%T) $*"; }
[ -w $RR ] && [ -w $HM ] || { say "no region_reads or held_mark: not the kernel from the tip"; exit 1; }
snap() {	# snap <dir> <tag>
	for f in svc_tcp_rx_budget_stats svc_tcp_rx_budget_read_stats; do
		cat /sys/module/sunrpc/parameters/$f > $1/$f.$2; done
	cat /sys/kernel/debug/page_pool_rx_region > $1/region.$2
}
delta() {	# delta <dir> <file>: the READ counters' change over the run
	paste -d' ' <(tr ' ' '\n' < $1/$2.0 | paste - -) <(tr ' ' '\n' < $1/$2.1 | paste - -) |
	awk '$1 ~ /^read_(admitted|parked|unconfined|short)$|^share_stalls$/ {printf "%s %d, ", $1, $4 - $2}'
}
for pair in "$@"; do
	IFS=: read n rr <<< "$pair"
	O=$L/t2-r$n; mkdir -p $O
	echo $rr > $RR && echo Y > $HM || exit 1
	snap $O 0
	./read-check.sh $n; rc=$?
	snap $O 1
	say "t2-r$n region_reads $(cat $RR): $(grep -E '^total' $O/run.log) | $(grep 'region pages in use at most' $O/run/summary.txt)"
	echo "    conn budget: $(delta $O svc_tcp_rx_budget_stats)"
	echo "    read pool:   $(delta $O svc_tcp_rx_budget_read_stats)"
	echo "    region: $(grep -E '^(read_pages|alloc_fail|escaped|max_held) ' $O/region.1 | paste -sd ' ')"
	# A rate under the mark goes on to the next run; a run that did not
	# finish, or one with a new kernel warning, stops the A/B.
	grep -q '^total' $O/run.log &&
		[ "$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')" = 0 ] ||
		{ say "t2-r$n did not finish clean (read-check.sh exited $rc); stopping"; break; }
done
echo N > $RR; echo N > $HM
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
