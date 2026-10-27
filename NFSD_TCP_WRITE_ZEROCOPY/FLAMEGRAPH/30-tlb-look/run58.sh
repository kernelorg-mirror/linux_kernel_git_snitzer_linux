#!/bin/bash
# run58.sh: round 30's measure, the B side: build 499 booted with
# rodata=noalias (the linear map in 32 MiB blocks), everything else as
# round 28's B (xeu 2C05DF674C9D013185FDB34 from updates/ with rx_prefetch
# Y, build 499's own sunrpc and nfsd, the settled setting).  The A side is
# round 28's r55 B runs and round 30's look (t2-r52, t2-r54), on rodata=full.
# Loan mode only, no copy runs (Mike).  lat.sh twice; four untraced
# loan64 runs; then one
# loan64 run with round 30's look (perf stat and sampled dtlb_walk on the
# ring CPUs) and perf stat on nfsd's CPUs beside it.  Each after
# settled.sh apply; stops at a new WARNING in dmesg or the first failure.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r58
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
XB=2C05DF674C9D013185FDB34 N=3F0C341E86E266D5FDF2ADF K=EFD53C53F61C1F0CDF37D89
XP=/sys/module/xeu/parameters/rx_prefetch
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
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
	[ "$(cat $XP)" = Y ] || die "$1: rx_prefetch $(cat $XP)"
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
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion) $(modinfo -n xeu)"
for l in L58-1 L58-2; do
	load $l
	snap > $O/$l-snap0.txt
	./lat.sh $O $l > $O/$l.run.txt 2>&1 || die "$l: $(tail -2 $O/$l.run.txt)"
	snap > $O/$l-snap1.txt
	say "$l | $(grep -h completion $O/$l-u.lat)"
done
for i in 1 2 3 4; do
	load r58-$i-loan64; run r58-$i-loan64 WRITES=64
done
# The look, as round 30's run57look.sh, with nfsd's CPUs (8-55) beside the rings.
t=r58-look-loan64
load $t
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 125
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w)
  perf stat -C 8-55 -e cycles,dtlb_walk,dtlb-load-misses,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,stall_backend_mem -o $O/stat-nfsd.txt -- sleep 4 &
  perf stat -C 0,2,4,6 -e cycles,dtlb_walk,dtlb-load-misses,l1d_cache_refill_rd,l2d_cache_refill_rd,ll_cache_miss_rd,stall_backend_mem -o $O/stat-rings.txt -- sleep 4
  wait; w1=$(w)
  echo "WRITEs in the stat: $((w1 - w0))" | tee -a $O/stat-rings.txt >> $O/stat-nfsd.txt
  perf record -C 0,2,4,6 -e dtlb_walk -c 200 -o $O/walk.data -- sleep 4 > /dev/null 2>&1 ) &
run $t WRITES=64
wait
say "$t | $(cat $O/look.when)"
perf report -i $O/walk.data --stdio --sort sym 2> /dev/null | grep -v '^#' | grep . | head -25 > $O/walk-top.txt
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
