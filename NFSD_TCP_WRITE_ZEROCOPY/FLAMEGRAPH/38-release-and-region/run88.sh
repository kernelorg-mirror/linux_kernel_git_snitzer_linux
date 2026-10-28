#!/bin/bash
# run88.sh: round 38's A/B of prefetching the release's pages twice, the
# fallback of "SUNRPC: Fetch a released page's struct page a second time"
# where CONFIG_ARM64 is not set.  One sunrpc, 84B04E2B2F2BC0495CE5C43 (the
# tip with sunrpc-ab-release-again.patch): A, svc_tcp_rx_rel_again 0
# (prefetchw a batch ahead only, the tip before that commit); B, 2
# (prefetchw again 4 entries ahead, as the build without CONFIG_ARM64 does).
# Four untraced loan64 runs a side, A B A B A B A B, at the current state
# (xeu 63631721F8B149913C0DC19, nfsd the kernel's, rodata=noalias, the
# settled setting).  cost.sh gives each run's ring and nfsd cycles a WRITE,
# and each run reports the region's peak.  The A/B's sunrpc is put in place
# by swap-sunrpc.sh first, and the tip's E80412A536ADD1E86B16F5A again at
# the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r88
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=84B04E2B2F2BC0495CE5C43 KT=E80412A536ADD1E86B16F5A
KT2=/sys/module/sunrpc/parameters/svc_tcp_rx_rel_again
say() { echo "$(date +%T) $*"; }
swap() {	# swap DIR: put DIR's sunrpc in place, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	./swap-sunrpc.sh $1 > $O/swap-$(basename $1).txt 2>&1 || return 1
	./settled.sh apply > $O/apply-swap-$(basename $1).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || swap $L/t2-r36-kept; exit 1; }
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
swap $L/t2-r38-again || die "the A/B's sunrpc not in place or not settled: $(tail -3 $O/swap-t2-r38-again.txt)"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
i=0
for v in A B A B A B A B; do
	i=$((i + 1)); tag=r88-$(( (i + 1) / 2 ))-loan64-$v
	load $tag
	echo $([ $v = B ] && echo 2 || echo 0) > $KT2
	[ "$(cat $KT2)" = "$([ $v = B ] && echo 2 || echo 0)" ] || die "$tag: svc_tcp_rx_rel_again $(cat $KT2)"
	run $tag
	say "$tag | svc_tcp_rx_rel_again $(cat $KT2)"
done
echo 1 > $KT2
swap $L/t2-r36-kept || die "the tip's sunrpc not in place"
[ "$(cat /sys/module/sunrpc/srcversion)" = $KT ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $KT"
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-swap-t2-r36-kept.txt); DONE"
