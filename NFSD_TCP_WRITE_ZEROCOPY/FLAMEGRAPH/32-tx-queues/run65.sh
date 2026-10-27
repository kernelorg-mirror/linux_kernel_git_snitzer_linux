#!/bin/bash
# run65.sh: round 32, more transmit queues for nfsd's sends.  Three sides,
# interleaved, four loan64 runs each (loan mode only, no copy runs):
#   S  the settled setting: 4 rings, tx-i's XPS on CPU 8+i only, so 44 of
#      nfsd's 48 CPUs pick a queue by the flow's hash
#   A  4 rings, XPS=spread: 12 of nfsd's CPUs to each queue (the map alone)
#   B  8 rings, RX_RINGS=4, XPS=spread: frames still only to rings 0-3, 8
#      transmit queues, 6 of nfsd's CPUs to each, Tx-4..7 doubled up on
#      CPUs 1, 3, 5, 7 (rings 4-7 post receive buffers the card never fills)
# Then lat.sh with B's setting.  The current state: the tip's sunrpc
# (A06FE478658242A10E100FF), nfsd the kernel's, xeu 2C05DF674C9D013185FDB34
# with rx_prefetch Y, rodata=noalias.  Each run after settled.sh apply;
# stops at a new WARNING in dmesg or the first failure.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r65
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=2C05DF674C9D013185FDB34 N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
declare -A SIDE=([S]="" [A]="XPS=spread" [B]="RINGS=8 RX_RINGS=4 XPS=spread")
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
pp() { ethtool -S $PORT | awk -F: '/_rx_pp_(alloc_slow|recycle_ring_full|recycle_cached)/ {gsub(/ring_[0-9]+_rx_pp_| /, "", $1); s[$1] += $2} END {for (k in s) printf "%s=%s ", k, s[k]}'; }
irqs() { awk -v p=$PORT '$NF ~ "^"p"-(Rx|Tx)-" {s=0; for (i=2; i<=NF-3; i++) s+=$i; split($NF, n, "-"); t[n[2]]+=s} END {printf "rxirq=%d txirq=%d ", t["Rx"], t["Tx"]}' /proc/interrupts; }
# The transmit queues' stops, wakes and drops, and what rings 4-7 received.
txq() { ethtool -S $PORT | awk -F: '/_tx_tx_stop_q|_tx_tx_wake_q|_tx_sw_dropped|_tx_hw_dropped/ {k=$1; gsub(/ring_[0-9]+_tx_| /, "", k); s[k] += $2} /ring_[4-7]_rx_packets/ {r += $2} END {for (k in s) printf "%s=%s ", k, s[k]; printf "rx4to7=%d ", r}'; }
snap() { echo "$(date +%s.%N) writes=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) pool=$(awk '!/^#/{print $2","$3","$4","$5}' /proc/fs/nfsd/pool_stats) ctxt=$(for p in $(pgrep -x nfsd); do cat /proc/$p/status; done | awk '/^voluntary_ctxt/{v += $2} END {print v}') $(pp)$(irqs)$(txq)outsegs=$(awk '/^Tcp:/ && $2 ~ /^[0-9]/ {print $12}' /proc/net/snmp) drc=$(awk -F: '/^(max entries|num entries|cache hits)/ {gsub(/ /, "", $1); gsub(/ /, "", $2); printf "%s:%s,", $1, $2}' /proc/fs/nfsd/reply_cache_stats)"; }
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
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
	snap > $O/$tag-snap1.txt
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
}

W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
for i in 1 2 3 4; do
	for s in S A B; do
		t=r65-$i-$s; load $t; run $t ${SIDE[$s]}
	done
done
load lat-B
env ${SIDE[B]} ./lat.sh $O LB > $O/LB.run.txt 2>&1 || die "LB: $(tail -2 $O/LB.run.txt)"
say "LB | $(grep -h completion $O/LB-u.lat)"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); $(ethtool -l $PORT | awk '/^Combined:/ {v = $2} END {print v}') rings; DONE"
