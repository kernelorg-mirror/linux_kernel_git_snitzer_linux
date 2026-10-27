#!/bin/bash
# run68.sh: FLAMEGRAPH round 34's A/B.  One xeu (30470F8F8281F32E66A5F35:
# round 28's with rx_page_split), A its rx_page_split N (the ring's 8 KiB
# buffers through page_pool_alloc_frag(), as before), B Y (whole pages
# counted once by the ring), four untraced loan64 runs a side, A B A B A B
# A B, at the current state (the tip's sunrpc, rodata=noalias, the settled
# setting).  Each after settled.sh apply (which writes N), then that side's
# value; the receive region's state is kept before and after each run, so
# a page the change failed to give back shows.  Stops at a new WARNING in
# dmesg or the first failure.  Leaves rx_page_split N, settled.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r68
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=30470F8F8281F32E66A5F35 N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
XS=/sys/module/xeu/parameters/rx_page_split
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; unset EXP_XEU_rx_page_split; echo N > $XS; exit 1; }
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
run() {	# run TAG [env ...]
	local tag=$1 rc; shift
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	snap > $O/$tag-snap0.txt
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 "$@" ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	sleep 5; snap > $O/$tag-snap1.txt
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
}

W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
i=0
for v in A B A B A B A B; do
	i=$((i + 1)); tag=r68-$(( (i + 1) / 2 ))-loan64-$v
	load $tag
	case $v in A) echo N > $XS; unset EXP_XEU_rx_page_split ;; B) echo Y > $XS; export EXP_XEU_rx_page_split=Y ;; esac
	[ "$(cat $XS)" = "$([ $v = B ] && echo Y || echo N)" ] || die "$tag: rx_page_split $(cat $XS)"
	run $tag
	say "$tag | rx_page_split $(cat $XS)"
done
unset EXP_XEU_rx_page_split; echo N > $XS
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: rx_page_split $(cat $XS); $(tail -1 $O/apply-end.txt); DONE"
