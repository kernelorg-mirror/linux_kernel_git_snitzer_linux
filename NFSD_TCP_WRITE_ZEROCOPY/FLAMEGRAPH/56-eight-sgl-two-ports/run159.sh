#!/bin/bash
# run159.sh: eight connections a port at 640 KiB (Mike), to stop run158's ring drops at
# 750 KiB, twice, the memory traffic beside each.  It takes over from run158 once its
# settled run (s16) is in, before its second 750 KiB run starts its load.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
F=../FLAMEGRAPH/56-eight-sgl-two-ports/run158.out
until grep -qE ' s16 \|| failed|not settled| end: ' $F; do sleep 2; done
pkill -TERM -f '[r]un158\.sh'; sleep 2
pgrep -f 'twoport\.sh /var/tmp' > /dev/null && { echo "run158 had started another load"; exit 1; }
O=$L/t2-r159
mkdir -p $O
say() { echo "$(date +%T) $*"; }
DRV="nvme0n1 nvme1n1 nvme2n1 nvme3n1 nvme4n1 nvme5n1 nvme6n1 nvme7n1"
for c in n8-640-1:8:655360 n8-640-2:8:655360; do
	wr=128 e=128
	tag=${c%%:*}; nc=${c#*:}; nc=${nc%%:*}; rb=${c##*:}
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
	( timed_load $O/$tag 10 || exit 1; ./memcost-log.sh $O/$tag-mc 22 enp161s0f0,enp164s0f0 $DRV ) &
	env NCONN=$nc RCVBUF=$rb ENTRIES=$e WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=$wr ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	wait
	w=$(awk '/nfsd in the window/ {for (i=1;i<=NF;i++) if ($i=="WRITEs") {gsub(/,/,"",$(i+1)); print $(i+1)}}' $O/$tag/window.txt)
	r=$(awk -F, -v w=$w '$1 ~ /^CPU[0-9]+$/ && $4 ~ /cycles/ {c=substr($1,4)+0; if(c==0||c==2||c==4||c==6||c==56||c==58||c==60||c==62)r+=$2} END{printf "%.1fk",r/10/w/1e3}' $O/$tag/perf-stat-window.txt)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | fifo $(awk '$1=="a"||$1=="b"{printf "%s ",$5}' $O/$tag/window.txt)ringdrop $(awk '$1=="a"||$1=="b"{printf "%s ",$6}' $O/$tag/window.txt)| $(grep -o 'client retransmits.*' $O/$tag/window.txt) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | rings $r $(./cost.sh $O/$tag | grep -o 'nfsd [0-9.]*k cycles') | snd_wnd $(grep -oE 'snd_wnd:[0-9]+' $O/$tag/ss-client.txt | cut -d: -f2 | sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)+1]}') rwnd% $(grep -oE 'rwnd_limited:[0-9]+ms\([0-9.]+%\)' $O/$tag/ss-client.txt | sed 's/.*(//;s/%)//' | sort -n | awk '{a[NR]=$1} END{print a[int(NR/2)+1]}') | $(grep -E '^drives of' $O/$tag/summary.txt | grep -oE 'writing [0-9.]+ to [0-9.]+ GB/s, busiest [0-9]+%' | paste -sd';') | $(python3 ./memcost-read.py $O/$tag-mc enp161s0f0,enp164s0f0 2>&1 | tail -3 | paste -sd' ' | grep -oE 'region in use.*')"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
