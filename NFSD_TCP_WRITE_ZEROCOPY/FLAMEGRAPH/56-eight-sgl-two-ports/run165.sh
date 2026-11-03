#!/bin/bash
# run165.sh: round 56's baseline at the new two-port setting (12 connections a port,
# 512 KiB, rings of 128; budget 128 WRITEs): four untraced loan runs on both ports with
# the memory traffic beside each, two of lat.sh (the one-port iodepth 1 yardstick,
# comparable with round 50's), then one profile on both ports.  Copy is not run.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r165
mkdir -p $O
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=5DDCC44639FC73AF533513F
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
ready() {	# ready TAG: the settled setting, checked
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
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
grep -q gicv3_pseudo_nmi /proc/cmdline && die "booted with pseudo-NMI"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump is not armed"
lsmod | grep -q '_kunit ' && die "a KUnit suite is loaded"
W0=$(bad)
say "kernel $(uname -r), sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages), W0 $W0, two-port rcvbuf $TWO_RCVBUF nconn $TWO_NCONN"
for i in 1 2 3 4; do run r165-$i; done
for i in 1 2; do
	t=l$i
	ready $t
	./lat.sh $O $t > $O/$t.out 2>&1 || die "lat $t: $(tail -2 $O/$t.out)"
	say "$t: $(grep -h 'p50' $O/$t.out | head -1)"
done
ready fl56
WHICH=ab ./flame.sh $O fl56 128 > $O/fl56.out 2>&1 || die "flame: $(tail -2 $O/fl56.out)"
say "fl56: $(grep '^total' $O/fl56.log 2>/dev/null) | $(cat $O/fl56-flame/cost.txt | paste -sd' ')"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(bad)" = $W0 ] || die "a new kernel message"
say "ALL end: $(tail -1 $O/apply-end.txt); DONE"
