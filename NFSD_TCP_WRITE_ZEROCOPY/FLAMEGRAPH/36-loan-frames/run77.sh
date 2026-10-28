#!/bin/bash
# run77.sh: round 36, where the borrow's wait went.  r76's A/B took 2.75k
# instructions a WRITE off nfsd but no cycles beyond the spread.  One
# diagnostic sunrpc, 6F7B01243426DBA783BAE7E (r76's A/B one with a per-CPU
# timing of the struct page loads, svc_tcp_rx_diag_lat, not for keeping),
# one loan64 run at the current state (xeu 63631721F8B149913C0DC19, nfsd
# the kernel's, rodata=noalias, the settled setting).  From 5 s after the
# WRITEs pass 10,000 a second, on nfsd's CPUs (8 to 55):
#   - four 3 s windows with the timing off, svc_tcp_rx_lean_append N Y N Y,
#     each a perf stat (cycles, instructions, backend memory stall) and a
#     perf record of cycles at the same time, so the two sides' cycles are
#     set symbol by symbol against each other in one run;
#   - two 3 s windows with the timing on, N then Y: the latency of the
#     borrow's first load of each page's first word, of a load of a page
#     just timed, of the release's first load, and of a line of the CPU's
#     own (the floor).
# The tip's sunrpc is put back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r77
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=6F7B01243426DBA783BAE7E KT=A06FE478658242A10E100FF
P=/sys/module/sunrpc/parameters
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r76-base; exit 1; }

client_free || die "client not free"
swap $L/t2-r77-diag || die "the diagnostic sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r77-diag.txt)"
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
echo N > $P/svc_tcp_rx_lean_append; echo N > $P/svc_tcp_rx_diag_lat; echo 0 > $P/svc_tcp_rx_lat
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r77-look-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  i=0
  for v in N Y N Y; do
	i=$((i + 1)); n=w$i-$v
	echo $v > $P/svc_tcp_rx_lean_append; sleep 0.5
	w0=$(w)
	perf record -C 8-55 -e cycles -c 200000 -o $O/cycles-$n.data -- sleep 3 > /dev/null 2>&1 &
	perf stat -C 8-55 -e cycles,instructions,stall_backend_mem -o $O/stat-$n.txt -- sleep 3
	wait; w1=$(w)
	echo "WRITEs in the window: $((w1 - w0))" >> $O/stat-$n.txt
  done
  echo Y > $P/svc_tcp_rx_diag_lat
  for v in N Y; do
	echo $v > $P/svc_tcp_rx_lean_append; sleep 0.5
	echo 0 > $P/svc_tcp_rx_lat; w0=$(w); sleep 3; cat $P/svc_tcp_rx_lat > $O/lat-$v.txt; w1=$(w)
	echo "WRITEs in the window: $((w1 - w0))" > $O/lat-$v.writes
  done
  echo N > $P/svc_tcp_rx_diag_lat; echo N > $P/svc_tcp_rx_lean_append ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo N > $P/svc_tcp_rx_diag_lat; echo N > $P/svc_tcp_rx_lean_append
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || say "a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(cat $O/look.when)"
# Ranked and annotated while the diagnostic sunrpc is loaded, so its symbols resolve.
for f in $O/cycles-w*.data; do
	n=$(basename $f .data)
	perf report -i $f --stdio --sort sym -F overhead,sample,sym 2> /dev/null | grep -v '^#' | grep . | sort -k2 -rn | head -80 > $O/samples-$n.txt
	for s in svc_tcp_rx_borrow_segment svc_tcp_rx_append_ref svc_tcp_rx_walk_skb; do
		timeout 300 perf annotate -i $f --stdio --symbol=$s > $O/annotate-$n-$s.txt 2>&1
	done
done
swap $L/t2-r76-base || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r76-base.txt); DONE"
