#!/bin/bash
# run174.sh: round 60, sunrpc's own loan frames at the two-port setting on
# build 507, a look (round 36's run75 method).  One loan run on both ports
# (12 connections a port at 512 KiB, rings of 128, budget 128), the kernel's
# own sunrpc, nfsd and xeu.  From 5 s into the timed window, on nfsd's CPUs
# (8 to 55): perf stat of cycles, instructions and the cache events for 4 s,
# then 4 s of samples each of cycles, backend memory stall cycles and L1D
# read refills, ranked and annotated; last, 4 s of bpftrace counts of each
# frame's calls (kprobes, so after the samples).  The WRITEs in each window
# are kept, so samples and calls become a WRITE's.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r174
mkdir -p $O
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
K=2A512644C685341146A3A9B N=BB6D1949646AB051A8D99CC XB=CA0931345E46C929B11D01F
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump not armed"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r174-look
# name:event:period
EV="cycles:cycles:200000 stall_backend_mem:stall_backend_mem:100000 l1d_cache_refill_rd:l1d_cache_refill_rd:500"
F="svc_tcp_rx_append_ref svc_tcp_rx_borrow_segment svc_tcp_rx_release_refs svc_tcp_rx_put_run svc_tcp_rx_walk_skb svc_tcp_rx_prefetch_pages svc_tcp_rx_region_span svc_tcp_rx_skb_region_pages_after svc_tcp_recv_skb"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 5 || exit 1
  echo "look from $(date +%T)" > $O/look.when
  w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,dtlb_walk -o $O/stat-nfsd.txt -- sleep 4; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" >> $O/stat-nfsd.txt
  for e in $EV; do
	IFS=: read n ev c <<< "$e"
	w0=$(w); perf record -C 8-55 -e $ev -c $c -o $O/$n.data -- sleep 4 > /dev/null 2>&1; w1=$(w)
	echo "$n $((w1 - w0))" >> $O/record-writes.txt
  done
  p=$(for f in $F; do printf 'kprobe:%s,' $f; done)
  w0=$(w); timeout -s INT 6 bpftrace -e "${p%,} { @[func] = count(); } interval:s:4 { exit(); }" > $O/calls.txt 2>&1; w1=$(w)
  echo "WRITEs while counted: $((w1 - w0)) (bpftrace's start included)" >> $O/calls.txt
  echo "look ends $(date +%T)" >> $O/look.when ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=128 ./twoport.sh $O/$t ab > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(./cost.sh $O/$t) | $(cat $O/look.when | paste -sd' ')"
for e in $EV; do
	IFS=: read n ev c <<< "$e"
	perf report -i $O/$n.data --stdio --sort sym -F overhead,sample,sym 2> /dev/null | grep -v "^#" | grep . | sort -k2 -rn | head -60 > $O/samples-$n.txt
	for f in $F; do
		timeout 300 perf annotate -i $O/$n.data --stdio --symbol=$f > $O/annotate-$n-$f.txt 2>&1
	done
done
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
