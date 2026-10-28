#!/bin/bash
# run90.sh: round 39's step 2, what the receive side's struct page loads
# cost.  One diagnostic sunrpc, D09FD368FE300634356470E (the tip's
# DDE22984B5905541BB2CD35 with sunrpc-diag-receive-prefetch.patch, not for
# keeping), one loan64 run at the current state (xeu 63631721F8B149913C0DC19,
# nfsd the kernel's, rodata=noalias, the settled setting).  From 10 s into
# the timed load (timed_load), for each fetch before the region count in
# turn (svc_tcp_rx_rcv_ahead 0 prefetchw, the tip's; 1 none; 2 a load; 0
# again): 1 s to change, a 2 s perf stat on nfsd's CPUs (8 to 55) with the
# timing off, 2 s with svc_tcp_rx_diag_rcv 1 (the section timed whole), and
# 2 s with 2 (each page's loads timed as well), each read from
# svc_tcp_rx_rcv_lat.  The tip's sunrpc is put back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r90
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=D09FD368FE300634356470E KT=DDE22984B5905541BB2CD35
P=/sys/module/sunrpc/parameters
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r39-tip; exit 1; }

client_free || die "client not free"
swap $L/t2-r39-rcv || die "the diagnostic sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r39-rcv.txt)"
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
echo 0 > $P/svc_tcp_rx_diag_rcv; echo 0 > $P/svc_tcp_rx_rcv_ahead
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r90-look-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  window() {	# window FILE: WRITEs and the stamps of a window, its readout before
	echo "WRITEs in the window: $((w1 - w0))" >> $1
	echo "window from $t0 to $t1" >> $1
  }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "look from $(date +%T)" > $O/look.when
  i=0
  for a in 0 1 2 0; do
	i=$((i + 1)); echo $a > $P/svc_tcp_rx_rcv_ahead; sleep 1
	t0=$(date +%s.%N); w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem -o $O/stat-$i-a$a.txt -- sleep 2; w1=$(w); t1=$(date +%s.%N)
	window $O/stat-$i-a$a.txt
	for d in 1 2; do
		echo 0 > $P/svc_tcp_rx_rcv_lat; echo $d > $P/svc_tcp_rx_diag_rcv
		t0=$(date +%s.%N); w0=$(w); sleep 2; echo 0 > $P/svc_tcp_rx_diag_rcv; cat $P/svc_tcp_rx_rcv_lat > $O/lat-$i-a$a-d$d.txt; w1=$(w); t1=$(date +%s.%N)
		window $O/lat-$i-a$a-d$d.txt
	done
  done
  echo 0 > $P/svc_tcp_rx_rcv_ahead ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/svc_tcp_rx_diag_rcv; echo 0 > $P/svc_tcp_rx_rcv_ahead
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || say "a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(cat $O/look.when)"
for f in $O/lat-*-d1.txt; do say "$(basename $f .txt): $(head -1 $f) | $(grep '^WRITEs' $f) | $(grep '^section' $f | awk '{printf "section %.0f ns mean", $5 / $3}')"; done
swap $L/t2-r39-tip || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r39-tip.txt); DONE"
