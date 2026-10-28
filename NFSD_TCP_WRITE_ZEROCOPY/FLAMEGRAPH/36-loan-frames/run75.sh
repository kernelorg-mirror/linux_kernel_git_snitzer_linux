#!/bin/bash
# run75.sh: round 36, nfsd's own loan frames (svc_tcp_rx_append_ref,
# svc_tcp_rx_borrow_segment, svc_tcp_rx_release_refs), a look.  One loan64
# run at the current state (the tip's sunrpc, nfsd the kernel's, xeu
# 63631721F8B149913C0DC19, round 35's, rodata=noalias, the settled
# setting).  From 5 s after the WRITEs pass 10,000 a second, on nfsd's CPUs
# (8 to 55): perf stat of cycles, instructions and the cache events for 4 s,
# then 4 s of samples each of cycles, backend memory stall cycles and L1D
# read refills, ranked by symbol and annotated; last, 4 s of bpftrace
# counts of each frame's calls (kprobes, so after the samples).  The
# WRITEs in each window are kept, so samples and calls become a WRITE's.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r75
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
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
t=r75-look-loan64
# name:event:period
EV="cycles:cycles:200000 stall_backend_mem:stall_backend_mem:100000 l1d_cache_refill_rd:l1d_cache_refill_rd:500"
F="svc_tcp_rx_append_ref svc_tcp_rx_borrow_segment svc_tcp_rx_release_refs svc_tcp_rx_put_run svc_tcp_rx_walk_skb svc_tcp_rx_prefetch_pages"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,dtlb_walk -o $O/stat-nfsd.txt -- sleep 4; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" >> $O/stat-nfsd.txt
  for e in $EV; do
	IFS=: read n ev c <<< "$e"
	w0=$(w); perf record -C 8-55 -e $ev -c $c -o $O/$n.data -- sleep 4 > /dev/null 2>&1; w1=$(w)
	echo "$n $((w1 - w0))" >> $O/record-writes.txt
  done
  p=$(for f in $F; do printf 'kprobe:%s,' $f; done)
  w0=$(w); timeout -s INT 6 bpftrace -e "${p%,} { @[func] = count(); } interval:s:4 { exit(); }" > $O/calls.txt 2>&1; w1=$(w)
  echo "WRITEs while counted: $((w1 - w0)) (bpftrace's start included)" >> $O/calls.txt ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
for e in $EV; do
	IFS=: read n ev c <<< "$e"
	perf report -i $O/$n.data --stdio --sort sym 2> /dev/null | grep -v '^#' | grep . | head -40 > $O/top-$n.txt
	perf report -i $O/$n.data --stdio --sort sym -F overhead,sample,sym 2> /dev/null | grep -v "^#" | grep . | sort -k2 -rn | head -60 > $O/samples-$n.txt
	for f in $F; do
		[ -s $O/annotate-$n-$f.txt ] || timeout 300 perf annotate -i $O/$n.data --stdio --symbol=$f > $O/annotate-$n-$f.txt 2>&1
	done
done
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
