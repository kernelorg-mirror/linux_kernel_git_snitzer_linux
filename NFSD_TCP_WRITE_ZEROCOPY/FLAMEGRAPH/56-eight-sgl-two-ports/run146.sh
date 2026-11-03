#!/bin/bash
# run146.sh: what stands between 72 GB/s and 80 on the eight drives.  First the
# drives alone, all eight at once from local fio with no network, then the NFS
# load at 512 and 384 KiB with the memory traffic beside it (memcost-log.sh).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
while pgrep -f '[r]un145\.sh' > /dev/null; do sleep 5; done
O=$L/t2-r146
mkdir -p $O
say() { echo "$(date +%T) $*"; }
DRV="nvme0n1 nvme1n1 nvme2n1 nvme3n1 nvme4n1 nvme5n1 nvme6n1 nvme7n1"
ready() {
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply-$1.txt 2>&1 || { say "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"; exit 1; }
	sync
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
	sleep 120
}
# The drives alone: 1 MiB direct writes from nfsd's CPUs, split 256 KiB as NFS's are.
ready local
./memcost-log.sh $O/local 40 enp161s0f0,enp164s0f0 $DRV &
J=
for n in 0 1 2 3 4 5 6 7; do mkdir -p /hs/nvme$n/zcbudget; J="$J --name=d$n --directory=/hs/nvme$n/zcbudget --filename_format=zclocal.\$jobnum"; done
taskset -c 8-55 fio --rw=write --bs=1M --size=64G --direct=1 --ioengine=libaio --iodepth=32 --numjobs=4 --time_based --runtime=35 --group_reporting --output-format=json --output=$O/local.json $J > $O/local.out 2>&1
wait
rm -f /hs/nvme[0-7]/zcbudget/zclocal.*
say "local | $(python3 -c "import json;j=json.load(open('$O/local.json'));print(sum(x['write']['bw_bytes'] for x in j['jobs'])/1e9,'GB/s;',' '.join('%.1f'%(x['write']['bw_bytes']/1e9) for x in j['jobs']))") | memory $(awk -F, '$4 ~ /hnf_mc_reqs/ {print $2 * 64 / 2e9}' $O/local.cmn.txt | sort -n | awk '{a[NR]=$1} END{printf "%.1f", a[int(NR*3/4)]}') GB/s"
for rb in 524288 393216; do
	tag=m$rb
	ready $tag
	( timed_load $O/$tag 10 || exit 1; ./memcost-log.sh $O/$tag-mc 22 enp161s0f0,enp164s0f0 $DRV ) &
	env RCVBUF=$rb ENTRIES=128 WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
		{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
	wait
	say "$tag | $(grep '^total' $O/$tag.log) | ringdrop $(awk '$1=="a"||$1=="b"{printf "%s ",$6}' $O/$tag/window.txt)fifo $(awk '$1=="a"||$1=="b"{printf "%s ",$5}' $O/$tag/window.txt)| $(grep -o 'client retransmits.*' $O/$tag/window.txt) | $(python3 ./memcost-read.py $O/$tag-mc enp161s0f0,enp164s0f0 2>&1 | tail -3 | paste -sd' ' | cut -c1-200)"
done
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
