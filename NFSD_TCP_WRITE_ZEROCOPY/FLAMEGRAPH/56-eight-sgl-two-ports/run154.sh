#!/bin/bash
# run154.sh: where a loaned page spends its time, stage 5 split by whether the page's
# other buffer was still held at the put, at the settled two-port setting and 512 KiB x 256.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r154
mkdir -p $O
say() { echo "$(date +%T) $*"; }
for c in 393216:128 524288:256; do
	rb=${c%:*} e=${c#*:}; tag=pl$rb-$e
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	( timed_load $O/$tag 10 || exit 1; timeout -s INT 10 bpftrace pagelife.bt > $O/$tag-pagelife.txt 2>&1 ) &
	env RCVBUF=$rb ENTRIES=$e WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	wait
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -o 'region pages in use at most.*' $O/$tag.log) | $(./pagelife-read.py $O/$tag-pagelife.txt | grep -E 'pages held|^ +[0-9]' | sed 's/  */ /g' | paste -sd';')"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
