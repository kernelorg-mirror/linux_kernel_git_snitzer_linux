#!/bin/bash
# run127.sh: round 49, repeated on a boot without pseudo-NMI: nfsd's 512 threads on 48, 16 and 8 CPUs, loan64 at the
# settled setting on build 505, counted and untraced.  A is 8-55, B 8-23, C
# 8-15; all three hold 8-11, port a's XPS CPUs.  Each NVMe queue is one CPU's,
# so the drives' interrupts follow the threads.  A B C C B A C A B at
# saturation, then lat.sh A B C C B A.
# twoport.sh pins for the run and puts 8-55 back, as does settled.sh apply.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r127
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=666175BED7A343E376C1742
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
declare -A SET=([A]=8-55 [B]=8-23 [C]=8-15)
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
die() {
	say "STOPPED: $*"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && ./settled.sh apply > $O/apply-stopped.txt 2>&1
	say "after the stop: $(tail -1 $O/apply-stopped.txt 2>/dev/null)"
	exit 1
}
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(cat $PP/rx_region_stack)" = 64 ] || die "$1: rx_region_stack $(cat $PP/rx_region_stack), not 64"
	[ "$(reg pages)" = 32768 ] || die "$1: the region is $(reg pages) pages, not 32768"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
irqs() {	# over the node's whole life: NVMe (the four drives), IPIs, WRITEs
	echo "$(awk '$NF ~ /^nvme[4-7]q/ {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '$1 == "IPI0:" || $1 == "IPI1:" {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)"
}
run() {	# run TAG SET: one loan64 run
	local tag=$1 c=$2 rc f0 f1 i0 i1
	load $tag
	f0=$(reg alloc_fail); i0=$(irqs)
	( timed_load $O/$tag 10 || exit 1
	  for i in $(seq 50); do reg free; sleep 0.2; done > $O/$tag-free.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 NFSD_CPUS=$c ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail); i1=$(irqs)
	say "$tag | CPUs $c | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | mean in use $(awk '{s += 32768 - $1; n++} END {printf "%.0f", n ? s / n : -1}' $O/$tag-free.txt) | $(echo "$i0 $i1" | awk '{w = $6 - $3; printf "a WRITE: nvme irqs %.2f, IPIs %.2f", ($4 - $1) / w, ($5 - $2) / w}') | $(grep -o 'threads woken [0-9]*, context switches [0-9]*' $O/$tag/window.txt) | $(grep -o 'completion.*' $O/$tag/summary.txt | head -1) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt)"
}
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
grep -q gicv3_pseudo_nmi /proc/cmdline && die "booted with pseudo-NMI"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages)"
i=0
for a in A B C C B A C A B; do i=$((i + 1)); run r127-$i-$a ${SET[$a]}; done
i=0
for a in A B C C B A; do
	i=$((i + 1)); t=l$i-$a
	load $t
	NFSD_CPUS=${SET[$a]} ./lat.sh $O $t > $O/$t.out 2>&1 || die "lat $t: $(tail -2 $O/$t.out)"
	say "$t CPUs ${SET[$a]}: $(grep -hE 'p50' $O/$t.out | head -1)"
done
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "ALL end: $(tail -1 $O/apply-end.txt); DONE"
