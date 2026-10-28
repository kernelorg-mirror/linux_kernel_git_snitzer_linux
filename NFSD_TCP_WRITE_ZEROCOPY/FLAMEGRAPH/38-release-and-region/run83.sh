#!/bin/bash
# run83.sh: round 38, where in the release's loop its struct page misses
# fall.  One diagnostic sunrpc, 7F6A60512D78EEFE9110CFE (the tip's
# E80412A536ADD1E86B16F5A with sunrpc-diag-release-by-position.patch, not
# for keeping), one loan64 run at the current state (xeu
# 63631721F8B149913C0DC19, nfsd the kernel's, rodata=noalias, the settled
# setting).  From 5 s after the WRITEs pass 10,000 a second, on nfsd's CPUs
# (8 to 55): a 3 s perf stat with the timing off, then three 3 s windows
# with svc_tcp_rx_diag_rel on, each read from svc_tcp_rx_rel_lat: the
# release's first load of each page by the page's ordinal in the release,
# the same for the entries past the first batch, the load of the bvec entry
# a batch ahead, and a line of the CPU's own (the floor).  The tip's sunrpc
# is put back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r83
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=7F6A60512D78EEFE9110CFE KT=E80412A536ADD1E86B16F5A
P=/sys/module/sunrpc/parameters
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r36-kept; exit 1; }

client_free || die "client not free"
swap $L/t2-r38-rel || die "the diagnostic sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r38-rel.txt)"
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
echo N > $P/svc_tcp_rx_diag_rel; echo 0 > $P/svc_tcp_rx_rel_lat
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r83-look-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem -o $O/stat-off.txt -- sleep 3; w1=$(w)
  echo "WRITEs in the window: $((w1 - w0))" >> $O/stat-off.txt
  echo Y > $P/svc_tcp_rx_diag_rel
  for i in 1 2 3; do
	echo 0 > $P/svc_tcp_rx_rel_lat; w0=$(w); sleep 3; cat $P/svc_tcp_rx_rel_lat > $O/lat-w$i.txt; w1=$(w)
	echo "WRITEs in the window: $((w1 - w0))" > $O/lat-w$i.writes
  done
  echo N > $P/svc_tcp_rx_diag_rel ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo N > $P/svc_tcp_rx_diag_rel
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || say "a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(cat $O/look.when)"
for i in 1 2 3; do say "w$i: $(head -1 $O/lat-w$i.txt) | $(cat $O/lat-w$i.writes)"; done
swap $L/t2-r36-kept || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r36-kept.txt); DONE"
