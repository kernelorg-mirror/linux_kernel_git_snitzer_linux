#!/bin/bash
# run145.sh: the held buffer on two ports at rings of 128, the loan's load:
# 448 and 512 KiB from tardis1's two-port sizes, and 640 under the 750 that
# dropped frames (run144).  384 is run143.  settled.sh is not changed.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r145
mkdir -p $O
say() { echo "$(date +%T) $*"; }
for rb in 458752 524288 655360; do
	tag=rb$rb
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	env RCVBUF=$rb ENTRIES=128 WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	w=$(awk '/nfsd in the window/ {for (i=1;i<=NF;i++) if ($i=="WRITEs") {gsub(/,/,"",$(i+1)); print $(i+1)}}' $O/$tag/window.txt)
	r=$(awk -F, -v w=$w '$1 ~ /^CPU[0-9]+$/ && $4 ~ /cycles/ {c=substr($1,4)+0; if(c==0||c==2||c==4||c==6||c==56||c==58||c==60||c==62)r+=$2} END{printf "%.1fk",r/10/w/1e3}' $O/$tag/perf-stat-window.txt)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | fifo $(awk '$1=="a"||$1=="b"{printf "%s ",$5}' $O/$tag/window.txt)ringdrop $(awk '$1=="a"||$1=="b"{printf "%s ",$6}' $O/$tag/window.txt)| $(grep -o 'client retransmits.*' $O/$tag/window.txt) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | rings $r $(./cost.sh $O/$tag | grep -o 'nfsd [0-9.]*k cycles') | snd_wnd $(grep -oE 'snd_wnd:[0-9]+' $O/$tag/ss-client.txt | cut -d: -f2 | sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)+1]}') rwnd% $(grep -oE 'rwnd_limited:[0-9]+ms\([0-9.]+%\)' $O/$tag/ss-client.txt | sed 's/.*(//;s/%)//' | sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)+1]}') | $(grep -E '^drives of' $O/$tag/summary.txt | grep -oE 'writing [0-9.]+ to [0-9.]+ GB/s, busiest [0-9]+%' | paste -sd';')"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
