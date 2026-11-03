#!/bin/bash
# run166.sh: where a page of the region spends its time at the new two-port setting (12
# connections a port, 512 KiB, rings of 128), after run165's baseline: one loan run on
# both ports at its defaults with pagelife.bt for 10 s inside the window.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
while pgrep -f '[r]un165\.sh' > /dev/null; do sleep 5; done
O=$L/t2-r166
mkdir -p $O
say() { echo "$(date +%T) $*"; }
tag=pl-settled
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
sync
for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
sleep 120
( timed_load $O/$tag 10 || exit 1; timeout -s INT 10 bpftrace pagelife.bt > $O/$tag-pagelife.txt 2>&1 ) &
env WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
	{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
wait
say "$tag | $(grep '^total' $O/$tag.log) | $(grep -o 'region pages in use at most.*' $O/$tag.log)"
./pagelife-read.py $O/$tag-pagelife.txt
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
