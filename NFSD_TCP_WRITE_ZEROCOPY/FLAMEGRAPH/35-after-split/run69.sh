#!/bin/bash
# run69.sh: round 35, where round 34's frag drain's wait went, and what in
# the rings is left after it.  run67.sh's look again, on xeu
# DC23D10E104D370F14FB06E (rx_buf_size's pieces cut by the ring, round 34,
# kept), at the current state, so each figure stands beside run67's on xeu
# 2C05DF674C9D013185FDB34.  On the ring CPUs (0, 2, 4, 6): perf stat of the
# cache events for 4 s, then 4 s of samples each of backend memory stall
# cycles, L1D, L2 and last-level read misses, and cycles; then 4 s of stall
# samples on nfsd's CPUs (8 to 55), where the received pieces are freed.
# The WRITEs in each window are kept, so samples become events a WRITE.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r69
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=DC23D10E104D370F14FB06E N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
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
t=r69-look-loan64
# name:event:period:cpus
EV="stall_backend_mem:stall_backend_mem:100000:0,2,4,6 l1d_cache_refill_rd:l1d_cache_refill_rd:500:0,2,4,6 l2d_cache_refill_rd:l2d_cache_refill_rd:1000:0,2,4,6 ll_cache_miss_rd:ll_cache_miss_rd:1000:0,2,4,6 cycles:cycles:200000:0,2,4,6 nfsd-stall_backend_mem:stall_backend_mem:100000:8-55"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w); perf stat -C 0,2,4,6 -e cycles,stall_backend_mem,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,dtlb_walk -o $O/stat-rings.txt -- sleep 4; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" >> $O/stat-rings.txt
  for e in $EV; do
	IFS=: read n ev c cpus <<< "$e"
	w0=$(w); perf record -C $cpus -e $ev -c $c -o $O/$n.data -- sleep 4 > /dev/null 2>&1; w1=$(w)
	echo "$n $((w1 - w0))" >> $O/record-writes.txt
  done ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
# Annotated while xeu is still loaded, so its symbols resolve.
for e in $EV; do
	IFS=: read n ev c cpus <<< "$e"
	perf report -i $O/$n.data --stdio --sort sym 2> /dev/null | grep -v '^#' | grep . | head -40 > $O/top-$n.txt
	perf report -i $O/$n.data --stdio --sort sym -F overhead,sample,sym 2> /dev/null | grep -v "^#" | grep . | sort -k2 -rn | head -60 > $O/samples-$n.txt
	for f in $(awk '{print $NF}' $O/top-$n.txt | head -8) xeu_rx_refill.isra.0 xeu_rx_poll page_pool_alloc_pages page_pool_alloc_frag_netmem; do
		[ -s $O/annotate-$n-$f.txt ] || timeout 300 perf annotate -i $O/$n.data --stdio --symbol=$f > $O/annotate-$n-$f.txt 2>&1
	done
done
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
