#!/bin/bash
# run157.sh: run156 again, with xpt_qtime stamped (recordwait.bt now enables the dequeue
# tracepoint), at the settled two-port setting and at 512 KiB x 256.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r157
mkdir -p $O
say() { echo "$(date +%T) $*"; }
for c in 393216:128 524288:256; do
	rb=${c%:*} e=${c#*:}; tag=pl$rb-$e
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	( timed_load $O/$tag 10 || exit 1; timeout -s INT 10 bpftrace recordwait.bt > $O/$tag-recordwait.txt 2>&1 ) &
	env RCVBUF=$rb ENTRIES=$e WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	wait
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -o 'region pages in use at most.*' $O/$tag.log) | $(awk '/^@records/ {n = $2} /^@sum\[/ {sub(/^@sum\[/, ""); sub(/\]:/, ""); v = $NF; $NF = ""; s[$0] = v} END {for (k in s) printf "%s%.0f us; ", k, s[k] / n; printf "records %d", n}' $O/$tag-recordwait.txt)"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
