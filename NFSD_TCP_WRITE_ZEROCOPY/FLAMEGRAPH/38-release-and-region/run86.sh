#!/bin/bash
# run86.sh: round 38, kinds of prefetch for the release's deep misses.  One
# diagnostic sunrpc, 8F97DEB564CBBF076C97644 (the tip's
# E80412A536ADD1E86B16F5A with sunrpc-diag-release-prefetch-kinds.patch,
# not for keeping), one loan64 run at the current state (xeu
# 63631721F8B149913C0DC19, nfsd the kernel's, rodata=noalias, the settled
# setting), at the tip's distance of 16.  From 5 s after the WRITEs pass
# 10,000 a second, for each kind of fetch ahead in turn (svc_tcp_rx_rel_ahead
# 0 prefetchw, 3 PSTL2KEEP, 4 PLDL2KEEP, 5 prefetchw at 16 and again at 4,
# 6 PSTL2KEEP at 32 and prefetchw at 16, 0 again): 1 s to change, a 2 s perf
# stat on nfsd's CPUs (8 to 55) with the timing off, then 2 s with
# svc_tcp_rx_diag_rel on, read from svc_tcp_rx_rel_lat.  Each window's
# start and end are stamped, to see whether a window that counts few WRITEs
# (r84's third, r85's second) is short.  The tip's sunrpc is put back at
# the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r86
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=8F97DEB564CBBF076C97644 KT=E80412A536ADD1E86B16F5A
P=/sys/module/sunrpc/parameters
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r36-kept; exit 1; }

client_free || die "client not free"
swap $L/t2-r38-kinds || die "the diagnostic sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r38-kinds.txt)"
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
echo N > $P/svc_tcp_rx_diag_rel; echo 0 > $P/svc_tcp_rx_rel_lat; echo 16 > $P/svc_tcp_rx_rel_dist; echo 0 > $P/svc_tcp_rx_rel_ahead; echo 4 > $P/svc_tcp_rx_rel_dist2
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r86-look-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  i=0
  for d in 0 3 4 5 6 0; do
	i=$((i + 1)); echo $d > $P/svc_tcp_rx_rel_ahead; sleep 1
	t0=$(date +%s.%N); w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem -o $O/stat-$i-a$d.txt -- sleep 2; w1=$(w); t1=$(date +%s.%N)
	echo "WRITEs in the window: $((w1 - w0))" >> $O/stat-$i-a$d.txt
	echo "window from $t0 to $t1" >> $O/stat-$i-a$d.txt
	echo 0 > $P/svc_tcp_rx_rel_lat; echo Y > $P/svc_tcp_rx_diag_rel
	t0=$(date +%s.%N); w0=$(w); sleep 2; echo N > $P/svc_tcp_rx_diag_rel; cat $P/svc_tcp_rx_rel_lat > $O/lat-$i-a$d.txt; w1=$(w); t1=$(date +%s.%N)
	echo "WRITEs in the window: $((w1 - w0))" > $O/lat-$i-a$d.writes
	echo "window from $t0 to $t1" >> $O/lat-$i-a$d.writes
  done
  echo 16 > $P/svc_tcp_rx_rel_dist; echo 0 > $P/svc_tcp_rx_rel_ahead; echo 4 > $P/svc_tcp_rx_rel_dist2
  echo N > $P/svc_tcp_rx_diag_rel ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo N > $P/svc_tcp_rx_diag_rel; echo 16 > $P/svc_tcp_rx_rel_dist; echo 0 > $P/svc_tcp_rx_rel_ahead; echo 4 > $P/svc_tcp_rx_rel_dist2
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || say "a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(cat $O/look.when)"
for f in $O/lat-*-a*.txt; do b=${f%.txt}; say "$(basename $b): $(head -1 $f) | $(head -1 $b.writes) | $(awk '/cycles/{c=$1} /WRITEs in/{print c, "cycles,", $NF, "WRITEs untimed"}' ${b/lat-/stat-}.txt)"; done
swap $L/t2-r36-kept || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r36-kept.txt); DONE"
