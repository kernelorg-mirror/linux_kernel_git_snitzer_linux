#!/bin/bash
# run123.sh: round 47, where lazytime's instructions go: one loan64 run without
# it (EXP_LAZYTIME=nolazytime) and one with, each with perf sampling the
# instructions event on nfsd's CPUs for 5 s from 10 s into the timed load, since
# lazytime took 4.3k cycles a WRITE but only 0.8k instructions (run122).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r123
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=666175BED7A343E376C1742
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
declare -A STK=([S]=64)
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
die() {
	say "STOPPED: $*"; unset EXP_RX_REGION_STACK EXP_LAZYTIME
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && ./settled.sh apply > $O/apply-stopped.txt 2>&1
	say "after the stop: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-stopped.txt 2>/dev/null)"
	exit 1
}
load() {	# load TAG STACK: the settled setting with the stack at STACK, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	unset EXP_RX_REGION_STACK
	export EXP_LAZYTIME=$3
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(cat $PP/rx_region_stack)" = $2 ] || die "$1: rx_region_stack $(cat $PP/rx_region_stack), not $2"
	[ "$(reg pages)" = 32768 ] || die "$1: the region is $(reg pages) pages, not 32768"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
run() {	# run TAG WRITES STACK: one run, copy when WRITES is 0
	local tag=$1 w=$2 s=$3 rc f0 f1 lw=
	[ $w -gt 0 ] && lw="WRITES=$w"
	load $tag $s $4
	f0=$(reg alloc_fail)
	( timed_load $O/$tag 10 || exit 1
	  w0=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)
	  perf record -C 8-55 -g -e instructions -c 1000000 -o $O/$tag-perf.data -- sleep 5 > /dev/null 2>&1
	  echo "WRITEs $(($(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) - w0))" > $O/$tag-writes.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag $lw ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt || echo copy) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | stack $(cat $PP/rx_region_stack)"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ -e $PP/rx_region_stack ] || die "no page_pool.rx_region_stack in this kernel"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages)"
run r123-1-A 64 64 nolazytime
run r123-2-B 64 64 lazytime
unset EXP_LAZYTIME
unset EXP_RX_REGION_STACK EXP_LAZYTIME
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "end: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-end.txt); DONE"
