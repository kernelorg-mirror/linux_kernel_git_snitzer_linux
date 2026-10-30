#!/bin/bash
# run115.sh: round 45's step 4, more runs of B on build 504 beside run113:
# page_pool.rx_region_stack 64, loan64 three times, then copy once.  The
# rest as run113.  Then the settled setting again.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r115
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=666175BED7A343E376C1742
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
declare -A STK=([A]=0 [B]=64)
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
die() {
	say "STOPPED: $*"; unset EXP_RX_REGION_STACK
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
	if [ $2 = 0 ]; then unset EXP_RX_REGION_STACK; else export EXP_RX_REGION_STACK=$2; fi
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
	load $tag $s
	f0=$(reg alloc_fail)
	( timed_load $O/$tag 10 || exit 1
	  for i in $(seq 50); do reg free; sleep 0.2; done > $O/$tag-free.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag $lw ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | mean in use $(awk '{s += 32768 - $1; n++} END {printf "%.0f", n ? s / n : -1}' $O/$tag-free.txt) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt || echo copy) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | stack $(cat $PP/rx_region_stack)"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ -e $PP/rx_region_stack ] || die "no page_pool.rx_region_stack in this kernel"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages)"
i=0
for a in B B B; do i=$((i + 1)); run r115-$i-loan64-$a 64 ${STK[$a]}; done
for a in B; do i=$((i + 1)); run r115-$i-copy-$a 0 ${STK[$a]}; done
unset EXP_RX_REGION_STACK
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "end: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-end.txt); DONE"
