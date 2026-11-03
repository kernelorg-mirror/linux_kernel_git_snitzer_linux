#!/bin/bash
# run170.sh: round 55, cycles, instructions and L1 and L2 refills sampled on nfsd's CPUs (8-55)
# for 8 s in one loan run on both ports at the two-port setting, for the IPC and refills of
# the SGL walk's functions (pmu.py), and perf annotate of where they refill.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r170
mkdir -p $O
say() { echo "$(date +%T) $*"; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), want_eighths $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_want_eighths)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
tag=pmu
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
sync
for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
sleep 120
( timed_load $O/$tag 5 || exit 1
  perf record -q -C 8-55 -F 1999 -e cycles,instructions,l1d_cache_refill,l2d_cache_refill -o $O/pmu.data -- sleep 8 > $O/pmu.out 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
	{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
wait
say "$tag | $(grep '^total' $O/$tag.log) | $(ls -s $O/pmu.data)"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
perf script -i $O/pmu.data -F ip,sym,period,event 2>/dev/null > $O/pmu.script
../FLAMEGRAPH/55-nvme-sgl-walk/pmu.py $O/pmu.script > $O/pmu.txt; cat $O/pmu.txt
for f in blk_map_iter_next bio_split_io_at; do
	for e in l1d_cache_refill l2d_cache_refill; do
		perf annotate -i $O/pmu.data --stdio -e $e -s $f 2>/dev/null | grep -E '^\s+[0-9.]+ :' | sort -rn | head -5 | sed "s/^/$f $e /"
	done
done > $O/annotate.txt; cat $O/annotate.txt
