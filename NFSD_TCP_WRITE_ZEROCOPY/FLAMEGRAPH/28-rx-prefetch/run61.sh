#!/bin/bash
# run61.sh: FLAMEGRAPH round 28's A/B again, a spot check on the tip's
# sunrpc (round 26's build, A06FE478658242A10E100FF; round 28 ran on build
# 499's own) with rodata=noalias: one xeu (2C05DF674C9D013185FDB34), A its
# rx_prefetch N, B Y, two untraced loan64 runs a side, N Y N Y.  Each after
# settled.sh apply (which writes Y), then that side's value.  Stops at a new
# WARNING in dmesg or the first failure.  Leaves rx_prefetch Y, settled.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r61
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=2C05DF674C9D013185FDB34 N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
XP=/sys/module/xeu/parameters/rx_prefetch
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; unset EXP_XEU_rx_prefetch; echo Y > $XP; exit 1; }
pp() { ethtool -S $PORT | awk -F: '/_rx_pp_(alloc_slow|recycle_ring_full|recycle_cached)/ {gsub(/ring_[0-9]+_rx_pp_| /, "", $1); s[$1] += $2} END {for (k in s) printf "%s=%s ", k, s[k]}'; }
irqs() { awk -v p=$PORT '$NF ~ "^"p"-(Rx|Tx)-" {s=0; for (i=2; i<=NF-3; i++) s+=$i; split($NF, n, "-"); t[n[2]]+=s} END {printf "rxirq=%d txirq=%d ", t["Rx"], t["Tx"]}' /proc/interrupts; }
snap() { echo "$(date +%s.%N) writes=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) pool=$(awk '!/^#/{print $2","$3","$4","$5}' /proc/fs/nfsd/pool_stats) ctxt=$(for p in $(pgrep -x nfsd); do cat /proc/$p/status; done | awk '/^voluntary_ctxt/{v += $2} END {print v}') $(pp)$(irqs)outsegs=$(awk '/^Tcp:/ && $2 ~ /^[0-9]/ {print $12}' /proc/net/snmp) drc=$(awk -F: '/^(max entries|num entries|cache hits)/ {gsub(/ /, "", $1); gsub(/ /, "", $2); printf "%s:%s,", $1, $2}' /proc/fs/nfsd/reply_cache_stats)"; }
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
	dmesg | grep -q 'rx_prefetch fetches the wrong buffer' && die "$1: $(dmesg | grep 'rx_prefetch fetches' | tail -1)"
	return 0
}
run() {	# run TAG [env ...]
	local tag=$1 rc; shift
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	snap > $O/$tag-snap0.txt
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag "$@" ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	snap > $O/$tag-snap1.txt
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step"
}

W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
i=0
for v in A B A B; do
	i=$((i + 1)); tag=r61-$i-loan64-$v
	load $tag
	case $v in A) echo N > $XP; export EXP_XEU_rx_prefetch=N ;; B) echo Y > $XP; unset EXP_XEU_rx_prefetch ;; esac
	[ "$(cat $XP)" = "$([ $v = B ] && echo Y || echo N)" ] || die "$tag: rx_prefetch $(cat $XP)"
	run $tag WRITES=64
	say "$tag | rx_prefetch $(cat $XP)"
done
unset EXP_XEU_rx_prefetch; echo Y > $XP
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: rx_prefetch $(cat $XP); $(tail -1 $O/apply-end.txt); DONE"
