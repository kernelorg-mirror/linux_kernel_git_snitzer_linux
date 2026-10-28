#!/bin/bash
# run91.sh: round 39's step 3, a lead for the receive side's struct page
# fetch.  One sunrpc, 751A649B7263BAC9E031BFC (the tip's
# DDE22984B5905541BB2CD35 with sunrpc-ab-receive-next.patch: round 39's
# diagnostic and svc_tcp_rx_rcv_next; not for keeping), at the current state
# (xeu 63631721F8B149913C0DC19, nfsd the kernel's, rodata=noalias, the
# settled setting).  First a look as run90's: from 10 s into the timed load,
# for svc_tcp_rx_rcv_next 0, 1, 2 and 0 again, 1 s to change, a 2 s perf
# stat on nfsd's CPUs and 2 s with the section timed whole
# (svc_tcp_rx_diag_rcv 1).  Then the A/B, counted and untraced: twelve
# loan64 runs, four rounds of the three settings in turn, each round
# starting one later (0 1 2, 1 2 0, 2 0 1, 0 1 2).  The tip's sunrpc is put
# back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r91
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=751A649B7263BAC9E031BFC KT=DDE22984B5905541BB2CD35
P=/sys/module/sunrpc/parameters
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; echo 0 > $P/svc_tcp_rx_diag_rcv 2>/dev/null; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r39-tip; exit 1; }
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	grep -qw 'rodata=noalias' /proc/cmdline || die "$1: not booted with rodata=noalias"
	[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XB"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	echo 0 > $P/svc_tcp_rx_diag_rcv; echo 0 > $P/svc_tcp_rx_rcv_ahead
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
line() {	# line TAG: the run's summary
	say "$1 | $(grep '^total' $O/$1.log) | $(grep -c 'no drive stepped down' $O/$1.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$1/geometry.txt) placed | $(./cost.sh $O/$1) | peak $((32768 - $(sort -n $O/$1/region-min-free.txt | head -1)))"
}

client_free || die "client not free"
swap $L/t2-r39-next || die "the A/B's sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r39-next.txt)"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"

t=r91-look-loan64
load $t
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "look from $(date +%T)" > $O/look.when
  i=0
  for n in 0 1 2 0; do
	i=$((i + 1)); echo $n > $P/svc_tcp_rx_rcv_next; sleep 1
	w0=$(w); perf stat -C 8-55 -e cycles,instructions,stall_backend_mem -o $O/stat-$i-n$n.txt -- sleep 2; w1=$(w)
	echo "WRITEs in the window: $((w1 - w0))" >> $O/stat-$i-n$n.txt
	echo 0 > $P/svc_tcp_rx_rcv_lat; echo 1 > $P/svc_tcp_rx_diag_rcv
	w0=$(w); sleep 2; echo 0 > $P/svc_tcp_rx_diag_rcv; cat $P/svc_tcp_rx_rcv_lat > $O/lat-$i-n$n.txt; w1=$(w)
	echo "WRITEs in the window: $((w1 - w0))" >> $O/lat-$i-n$n.txt
  done
  echo 0 > $P/svc_tcp_rx_rcv_next ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/svc_tcp_rx_diag_rcv; echo 0 > $P/svc_tcp_rx_rcv_next
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
line $t; say "$(cat $O/look.when)"
for f in $O/lat-*.txt; do say "$(basename $f .txt): $(grep '^section n' $f | awk '{printf "section %.0f ns mean", $5 / $3}') | $(awk '/cycles$|cycles /{c=$1} /^WRITEs/{w=$5} END{printf "untimed %.1fk cycles a WRITE", c / w / 1000}' ${f/lat-/stat-})"; done

r=0
for order in "0 1 2" "1 2 0" "2 0 1" "0 1 2"; do
	r=$((r + 1))
	for n in $order; do
		tag=r91-$r-loan64-n$n
		load $tag
		echo $n > $P/svc_tcp_rx_rcv_next
		[ "$(cat $P/svc_tcp_rx_rcv_next)" = $n ] || die "$tag: svc_tcp_rx_rcv_next $(cat $P/svc_tcp_rx_rcv_next)"
		env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
		[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
		say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | svc_tcp_rx_rcv_next $(cat $P/svc_tcp_rx_rcv_next)"
	done
done
echo 0 > $P/svc_tcp_rx_rcv_next
swap $L/t2-r39-tip || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r39-tip.txt); DONE"
