#!/bin/bash
# ab-switch.sh SW OUT: the A/B for FLAMEGRAPH round 55 or 58 on the kernel built
# with "DROP: page_pool, block: A/B switches for FLAMEGRAPH rounds 55 and 58",
# on one boot, at the two-port setting (12 connections a port, 512 KiB, rings of
# 128, budget 128): four untraced loan runs on both ports, A B B A, with the
# memory traffic beside each, then lat.sh at iodepth 1 in both orders, A B B A
# then B A A B, and a profile of B.  Ends on B (the switch on, the kernel's default).
#   SW=pp   page_pool.rx_region_bulk_ab: A N, B Y (round 58)
#   SW=blk  blk_mq_dma.map_iter_inline: A N, B Y (round 55)
# Run it from the round's directory: ../ab-switch.sh pp t2-r171
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
SW=$1
O=$L/${2:?usage: ab-switch.sh pp|blk OUT}
mkdir -p $O
case $SW in
pp)  SP=/sys/module/page_pool/parameters/rx_region_bulk_ab ;;
blk) SP=/sys/module/blk_mq_dma/parameters/map_iter_inline ;;
*)   echo "usage: ab-switch.sh pp|blk OUT"; exit 1 ;;
esac
PP=/sys/module/page_pool/parameters
DRV="nvme0n1 nvme1n1 nvme2n1 nvme3n1 nvme4n1 nvme5n1 nvme6n1 nvme7n1"
say() { echo "$(date +%T) $*"; }
bad() { dmesg | grep -cE 'WARNING:|BUG:|Oops|rcu.*stall|soft lockup|blocked for more'; }
die() {
	say "STOPPED: $*"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && ./settled.sh apply > $O/apply-stopped.txt 2>&1
	say "after the stop: $(tail -1 $O/apply-stopped.txt 2>/dev/null)"
	exit 1
}
ready() {	# ready TAG: the settled setting, checked, and the arm TAG ends in
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(cat $PP/rx_region_stack)" = 64 ] || die "$1: rx_region_stack $(cat $PP/rx_region_stack), not 64"
	[ "$(reg pages)" = 32768 ] || die "$1: the region is $(reg pages) pages, not 32768"
	[ "$(bad)" = $W0 ] || die "$1: a new kernel message: $(dmesg | grep -E 'WARNING:|BUG:|Oops' | tail -3)"
	case $1 in *A) echo N > $SP ;; *B) echo Y > $SP ;; esac
	sync
}
irqs() {	# over the node's whole life: NVMe (the eight data drives), IPIs, WRITEs
	echo "$(awk '$NF ~ /^nvme[0-7]q/ {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '$1 == "IPI0:" || $1 == "IPI1:" {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)"
}
run() {	# run TAG: one untraced loan run on both ports at the settled two-port setting
	local tag=$1 rc f0 f1 i0 i1 w r
	ready $tag
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	f0=$(reg alloc_fail); i0=$(irqs)
	( timed_load $O/$tag 10 || exit 1; ./memcost-log.sh $O/$tag-mc 22 enp161s0f0,enp164s0f0 $DRV ) &
	env WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail); i1=$(irqs)
	w=$(awk '/nfsd in the window/ {for (i=1;i<=NF;i++) if ($i=="WRITEs") {gsub(/,/,"",$(i+1)); print $(i+1)}}' $O/$tag/window.txt)
	r=$(awk -F, -v w=$w '$1 ~ /^CPU[0-9]+$/ && $4 ~ /cycles/ {c=substr($1,4)+0; if(c==0||c==2||c==4||c==6||c==56||c==58||c==60||c==62)r+=$2} END{printf "%.1fk",r/10/w/1e3}' $O/$tag/perf-stat-window.txt)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | rings $r $(./cost.sh $O/$tag | grep -o 'nfsd [0-9.]*k cycles [0-9.]*k instructions') | ringdrop $(awk '$1=="a"||$1=="b"{printf "%s ",$6}' $O/$tag/window.txt)fifo $(awk '$1=="a"||$1=="b"{printf "%s ",$5}' $O/$tag/window.txt)| $(echo "$i0 $i1" | awk '{w = $6 - $3; printf "a WRITE: nvme irqs %.2f, IPIs %.2f", ($4 - $1) / w, ($5 - $2) / w}') | $(grep -o 'threads woken [0-9]*, context switches [0-9]*' $O/$tag/window.txt) | $(grep -o 'completion.*' $O/$tag/summary.txt | head -1) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | $(python3 ./memcost-read.py $O/$tag-mc enp161s0f0,enp164s0f0 2>&1 | tail -3 | paste -sd' ' | grep -oE 'region in use.*' | cut -c1-170)"
}
[ -e $SP ] || die "this kernel has no $SP: not the kernel with the A/B switches"
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
grep -q gicv3_pseudo_nmi /proc/cmdline && die "booted with pseudo-NMI"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump is not armed"
lsmod | grep -q '_kunit ' && die "a KUnit suite is loaded"
# This kernel's own modules: their versions at the start are the ones checked.
XK=$(cat /sys/module/xeu/srcversion) N=$(cat /sys/module/nfsd/srcversion) K=$(cat /sys/module/sunrpc/srcversion)
W0=$(bad)
say "kernel $(uname -r), $SP, sunrpc $K, nfsd $N, xeu $XK, region $(reg pages), W0 $W0, two-port rcvbuf $TWO_RCVBUF nconn $TWO_NCONN"
for t in 1-A 2-B 3-B 4-A; do run $SW-$t; done
for t in l1-A l2-B l3-B l4-A l5-B l6-A l7-A l8-B; do
	ready $t
	./lat.sh $O $t > $O/$t.out 2>&1 || die "lat $t: $(tail -2 $O/$t.out)"
	say "$t: $(grep -h 'p50' $O/$t.out | head -1)"
done
ready fl-B
WHICH=ab ./flame.sh $O fl-$SW 128 > $O/fl.out 2>&1 || die "flame: $(tail -2 $O/fl.out)"
say "fl-$SW (B): $(grep '^total' $O/fl-$SW.log 2>/dev/null) | $(cat $O/fl-$SW-flame/cost.txt | paste -sd' ')"
echo Y > $SP
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(bad)" = $W0 ] || die "a new kernel message"
say "ALL end: $SP $(cat $SP); $(tail -1 $O/apply-end.txt); DONE"
