#!/bin/bash
# run67.sh: round 34, where the rings' memory stall is.  Round 30 left it at
# 20% of the ring CPUs' cycles on rodata=noalias, cache misses and not TLB
# walks.  One loan64 run at the current state (the tip's sunrpc, nfsd the
# kernel's, xeu 2C05DF674C9D013185FDB34 with rx_prefetch Y, rodata=noalias,
# the settled setting); from 5 s after the WRITEs pass 10,000 a second, on
# the ring CPUs (0, 2, 4, 6): perf stat of the cache events for 4 s, then
# 4 s of samples each of backend memory stall cycles, L1D read refills, L2
# read refills and last-level read misses, ranked by symbol and annotated
# while xeu is loaded.  It also says whether round 33's completion entry
# still ranks in xeu_rx_poll's refills (FLAMEGRAPH round 33).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r67
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=2C05DF674C9D013185FDB34 N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r67-look-loan64
EV="stall_backend_mem:100000 l1d_cache_refill_rd:500 l2d_cache_refill_rd:1000 ll_cache_miss_rd:1000"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w); perf stat -C 0,2,4,6 -e cycles,stall_backend_mem,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,dtlb_walk -o $O/stat-rings.txt -- sleep 4; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" >> $O/stat-rings.txt
  for e in $EV; do
	perf record -C 0,2,4,6 -e ${e%:*} -c ${e#*:} -o $O/${e%:*}.data -- sleep 4 > /dev/null 2>&1
  done ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
# Annotated while xeu is still loaded, so its symbols resolve.
for e in $EV; do
	e=${e%:*}
	perf report -i $O/$e.data --stdio --sort sym 2> /dev/null | grep -v '^#' | grep . | head -30 > $O/top-$e.txt
	for f in $(awk '{print $NF}' $O/top-$e.txt | head -6) xeu_rx_poll; do
		[ -s $O/annotate-$e-$f.txt ] || timeout 300 perf annotate -i $O/$e.data --stdio --symbol=$f > $O/annotate-$e-$f.txt 2>&1
	done
done
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
