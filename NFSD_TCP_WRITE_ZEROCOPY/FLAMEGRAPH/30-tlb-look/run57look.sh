#!/bin/bash
# run57look.sh: FLAMEGRAPH round 30, where the ring's prefetch of the next
# frame waits: one loan64 run at the settled setting (xeu rx_prefetch Y,
# build 499's own sunrpc), and from 5 s after the WRITEs pass 10,000 a
# second, on the ring CPUs (0, 2, 4, 6): perf stat of TLB and cache events
# for 4 s, then samples of dtlb_walk and of l1d_cache_refill_rd for 4 s
# each, annotated while xeu is loaded.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r54
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), xeu $(cat /sys/module/xeu/srcversion) rx_prefetch $(cat /sys/module/xeu/parameters/rx_prefetch)"
client_free || die "client not free"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
[ "$(cat /sys/module/xeu/parameters/rx_prefetch)" = Y ] || die "rx_prefetch not Y"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  for t in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w); perf stat -C 0,2,4,6 -e cycles,dtlb_walk,dtlb-load-misses,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,stall_backend_mem -o $O/stat-rings.txt -- sleep 4; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" >> $O/stat-rings.txt
  perf record -C 0,2,4,6 -e dtlb_walk -c 200 -o $O/walk.data -- sleep 4 > /dev/null 2>&1
  perf record -C 0,2,4,6 -e l1d_cache_refill_rd -c 500 -o $O/refill.data -- sleep 4 > /dev/null 2>&1 ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=k57-loan64 WRITES=64 ./twoport.sh $O/k57-loan64 a > $O/k57-loan64.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "k57: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/k57-loan64.log | head -3)"
say "k57-loan64 | $(grep '^total' $O/k57-loan64.log) | $(cat $O/look.when)"
for d in walk refill; do
	perf report -i $O/$d.data --stdio --sort sym 2> /dev/null | grep -v '^#' | grep . | head -25 > $O/$d-top.txt
	for f in xeu_rx_poll eth_type_trans napi_build_skb __build_skb_around skb_gro_receive gro_list_prepare; do
		timeout 300 perf annotate -i $O/$d.data --stdio --symbol=$f > $O/$d-annotate-$f.txt 2>&1
	done
done
say "annotated; DONE"
