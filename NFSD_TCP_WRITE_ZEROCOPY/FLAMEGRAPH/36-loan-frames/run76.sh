#!/bin/bash
# run76.sh: round 36's A/B.  One sunrpc, D396F4A3FDA305DA0A0756C (the tip's
# A06FE478658242A10E100FF with changes 1 and 2 behind
# svc_tcp_rx_lean_append, read at each call): A N (the tip's append and
# borrow), B Y (a page's region check skipped when it is the page just
# counted, the coalesce test's page and offset compared before its bitmaps,
# and the borrow's page-pool test from one load of the page's word), four
# untraced loan64 runs a side, A B A B A B A B, at the current state (xeu
# 63631721F8B149913C0DC19, nfsd the kernel's, rodata=noalias, the settled
# setting).  cost.sh gives each run's ring and nfsd cycles a WRITE.  The
# A/B's sunrpc is put in place by swap-sunrpc.sh first, and the tip's again
# at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r76
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=D396F4A3FDA305DA0A0756C KT=A06FE478658242A10E100FF
KL=/sys/module/sunrpc/parameters/svc_tcp_rx_lean_append
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r76-base; exit 1; }
pp() { ethtool -S $PORT | awk -F: '/_rx_pp_(alloc_slow|alloc_fast|recycle_ring_full|recycle_cached)/ {gsub(/ring_[0-9]+_rx_pp_| /, "", $1); s[$1] += $2} END {for (k in s) printf "%s=%s ", k, s[k]}'; }
snap() { echo "$(date +%s.%N) writes=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) $(pp)"; cat $RG; }
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
	return 0
}
run() {	# run TAG
	local tag=$1 rc
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	snap > $O/$tag-snap0.txt
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	sleep 5; snap > $O/$tag-snap1.txt
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
}

W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
swap $L/t2-r76-ab || die "the A/B's sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r76-ab.txt)"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
i=0
for v in A B A B A B A B; do
	i=$((i + 1)); tag=r76-$(( (i + 1) / 2 ))-loan64-$v
	load $tag
	echo $([ $v = B ] && echo Y || echo N) > $KL
	[ "$(cat $KL)" = "$([ $v = B ] && echo Y || echo N)" ] || die "$tag: svc_tcp_rx_lean_append $(cat $KL)"
	run $tag
	say "$tag | svc_tcp_rx_lean_append $(cat $KL)"
done
echo N > $KL
swap $L/t2-r76-base || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r76-base.txt); DONE"
