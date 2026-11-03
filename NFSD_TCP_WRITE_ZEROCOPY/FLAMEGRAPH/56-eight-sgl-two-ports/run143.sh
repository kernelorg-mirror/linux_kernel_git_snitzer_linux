#!/bin/bash
# run143.sh: the baseline of tardis2's eight SGL drives over both ports, the
# region on both xeu devices: four untraced loan runs of 1 MiB WRITEs, 128
# WRITEs of budget (round 50's 64 for each four drives).  Copy is not run.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r143
mkdir -p $O
say() { echo "$(date +%T) $*"; }
bad() { dmesg | grep -cE 'WARNING:|BUG:|Oops|rcu.*stall|soft lockup|blocked for more'; }
W0=$(bad)
for r in 1 2 3 4; do
	tag=b$r
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	[ "$(bad)" = $W0 ] || { say "$tag: a new kernel message"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	f0=$(reg alloc_fail)
	( timed_load $O/$tag 10 || exit 1
	  for i in $(seq 50); do reg free; sleep 0.2; done > $O/$tag-free.txt ) &
	env WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || { say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | mean in use $(awk '{s += 32768 - $1; n++} END {printf "%.0f", n ? s / n : -1}' $O/$tag-free.txt) | alloc_fail $(( $(reg alloc_fail) - f0 )) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt)"
done
say done
