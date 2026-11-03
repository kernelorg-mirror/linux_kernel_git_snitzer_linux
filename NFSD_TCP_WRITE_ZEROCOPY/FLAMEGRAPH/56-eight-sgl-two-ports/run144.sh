#!/bin/bash
# run144.sh: after run143, is the receive window the limit on two ports?  The
# same load with each port at the one-port setting's 750 KiB held buffer and
# rings of 256, against the two-port setting's 384 KiB and 128.  An experiment:
# settled.sh is not changed.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
while pgrep -f run143.sh > /dev/null; do sleep 5; done
O=$L/t2-r144
mkdir -p $O
say() { echo "$(date +%T) $*"; }
for r in 1 2; do
	tag=w$r
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	env RCVBUF=768000 ENTRIES=256 WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | $(grep -E '^(drives of|port )' $O/$tag/summary.txt | cut -c1-90 | paste -sd';')"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
