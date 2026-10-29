#!/bin/bash
# run112.sh: round 45's step 3, where a shallow stack's cycles go in loan
# mode.  run111's B (rx_region_stack 64) cost loan64 3.9k ring and 10.5k nfsd
# cycles a WRITE against A (0, the ring's 256), for pages that find their
# pool's stack full and go round through the region: unmapped, their DMA
# index erased, put on the region's free list under its lock, taken back
# under it, mapped and indexed again.  Four loan64 runs, A B B A, the kept
# xeu, the region at 32768, the settled setting.  From 10 s into each timed
# load:
#   1. 5 s of perf record on CPUs 0-55, one sample every 1M cycles, with
#      call chains: each round-trip function's cycles a WRITE with what it
#      calls, on the ring CPUs (0-7) and on nfsd's (8-55) apart;
#   2. 5 s of bpftrace counting calls into each, by the same groups: the
#      pages a WRITE that go round, and how often each step runs.
# The runs are perturbed by both, so their own cycle counts are not the A/B;
# run111's are.  Then the settled setting again.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r112
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
declare -A STK=([A]=0 [B]=64)
PERIOD=1000000
# The round trip and what it calls, and the stack's own push and refill
FN="page_pool_put_netmem_bulk page_pool_recycle_ring_bulk page_pool_return_netmem page_pool_rx_region_put dma_unmap_page_attrs __xa_erase page_pool_rx_stack_refill __page_pool_alloc_netmems_slow page_pool_rx_region_alloc page_pool_dma_map dma_map_page_attrs page_pool_register_dma_index __xa_alloc _raw_spin_lock_irqsave queued_spin_lock_slowpath"
PROBES="page_pool_put_netmem_bulk page_pool_recycle_ring_bulk page_pool_return_netmem page_pool_rx_region_put page_pool_rx_stack_refill page_pool_rx_region_alloc page_pool_dma_map"
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
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
look() {	# look DIR TAG: the profile, then the counts, in the run's window
	local O=$1 t=$2 w0 w1
	w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
	timed_load $O/$t 10 || { echo "the timed load did not start" > $O/$t-look.when; return 1; }
	echo "look from $(date +%T)" > $O/$t-look.when
	w0=$(w); perf record -C 0-55 -g -e cycles -c $PERIOD -o $O/$t-perf.data -- sleep 5 > /dev/null 2>&1; w1=$(w)
	echo "WRITEs $((w1 - w0))" > $O/$t-perf-writes.txt
	w0=$(w)
	bpftrace -e "$(for f in $PROBES; do printf 'kprobe:%s ' $f; done | sed 's/ $//; s/ /,/g') { @[probe, cpu < 8 ? \"ring\" : \"nfsd\"] = count(); } interval:s:5 { exit(); }" > $O/$t-calls.txt 2>&1
	w1=$(w); echo "WRITEs $((w1 - w0))" >> $O/$t-calls.txt
	echo "end $(date +%T)" >> $O/$t-look.when
}
report() {	# report DIR TAG: cycles a WRITE in each function, with its callees; calls a WRITE
	local O=$1 t=$2 g c w
	w=$(awk '{print $2}' $O/$t-perf-writes.txt)
	for g in ring:0-7 nfsd:8-55; do
		perf report -i $O/$t-perf.data --cpu ${g#*:} --children --sort symbol -g none --percent-limit 0 --stdio 2> /dev/null > $O/$t-children-${g%%:*}.txt
		c=$(awk '/^# Event count/ {print $NF}' $O/$t-children-${g%%:*}.txt)
		for f in $FN; do
			awk -v f=$f -v c=$c -v w=$w -v g=${g%%:*} '$4 == f {gsub(/%/, "", $1); gsub(/%/, "", $2); printf "%-5s %-34s %8.2fk cycles a WRITE with callees, %7.2fk self\n", g, f, $1 * c / 100 / w / 1000, $2 * c / 100 / w / 1000; found = 1}
				END {if (!found) printf "%-5s %-34s %8s\n", g, f, "-"}' $O/$t-children-${g%%:*}.txt
		done
	done
	awk '/^WRITEs/ {w = $2} /^@\[/ {gsub(/[@\[\]:,]/, " "); n[$3 " " $2] = $NF} END {for (k in n) {split(k, a, " "); printf "%-5s %-34s %8.2f calls a WRITE\n", a[1], a[2], n[k] / w}}' $O/$t-calls.txt
}
run() {	# run TAG STACK
	local tag=$1 s=$2 rc
	load $tag $s
	look $O $tag &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | stack $(cat $PP/rx_region_stack) | $(paste -sd ' ' $O/$tag-look.when)"
	report $O $tag | tee $O/$tag-report.txt
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages)"
i=0
for a in A B B A; do i=$((i + 1)); run r112-$i-loan64-$a ${STK[$a]}; done
unset EXP_RX_REGION_STACK
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "end: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-end.txt); DONE"
