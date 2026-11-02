#!/bin/bash
# run137.sh: round 53, the A/B of the table walk at release: A round 52's sunrpc,
# B with the walk skipped for a published record, sunrpc swapped before each run.
# A B B A B A A B at saturation, untraced, then lat.sh A B B A, then one profile
# on B.  Copy is not run.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r137
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=none
AB=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/d422d5e4-dcd9-4151-84e7-ab3e472a1d51/scratchpad/ab53
declare -A KV=([A]=5D39507E93127A6010C9B8E [B]=5DDCC44639FC73AF533513F)
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
say() { echo "$(date +%T) $*"; }
to() {	# to A|B: that sunrpc, and nfs-server on it
	K=${KV[$1]}
	if [ "$(cat /sys/module/sunrpc/srcversion)" != $K ]; then
		./swap-sunrpc.sh $AB/$1 > $O/swap.txt 2>&1 || die "swap $1: $(tail -2 $O/swap.txt)"
	fi
	systemctl start nfs-server || die "nfs-server"
	sleep 2
}
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
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
irqs() {	# over the node's whole life: NVMe (the four drives), IPIs, WRITEs
	echo "$(awk '$NF ~ /^nvme[2-5]q/ {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '$1 == "IPI0:" || $1 == "IPI1:" {for (i = 2; i <= 65; i++) s += $i} END {print s}' /proc/interrupts) $(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)"
}
run() {	# run TAG: one loan64 run
	local tag=$1 rc f0 f1 i0 i1
	ready $tag
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	f0=$(reg alloc_fail); i0=$(irqs)
	( timed_load $O/$tag 10 || exit 1
	  for i in $(seq 50); do reg free; sleep 0.2; done > $O/$tag-free.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail); i1=$(irqs)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c '^no stall' $O/$tag.log) no-stall | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | mean in use $(awk '{s += 32768 - $1; n++} END {printf "%.0f", n ? s / n : -1}' $O/$tag-free.txt) | $(echo "$i0 $i1" | awk '{w = $6 - $3; printf "a WRITE: nvme irqs %.2f, IPIs %.2f", ($4 - $1) / w, ($5 - $2) / w}') | $(grep -o 'threads woken [0-9]*, context switches [0-9]*' $O/$tag/window.txt) | $(grep -o 'completion.*' $O/$tag/summary.txt | head -1) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt)"
}
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
grep -q gicv3_pseudo_nmi /proc/cmdline && die "booted with pseudo-NMI"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump is not armed"
lsmod | grep -q '_kunit ' && die "a KUnit suite is loaded"
W0=$(bad)
say "kernel $(uname -r), A ${KV[A]}, B ${KV[B]}, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages), W0 $W0"
i=0
for a in A B B A B A A B; do i=$((i + 1)); to $a; run r137-$i-$a; done
i=0
for a in A B B A; do
	i=$((i + 1)); t=l$i-$a
	to $a
	ready $t
	./lat.sh $O $t > $O/$t.out 2>&1 || die "lat $t: $(tail -2 $O/$t.out)"
	say "$t: $(grep -hE 'p50' $O/$t.out | head -1)"
done
to B
ready fl53
./flame.sh $O fl53 > $O/fl53.out 2>&1 || die "flame: $(tail -2 $O/fl53.out)"
say "fl53: $(grep '^total' $O/fl53.log 2>/dev/null) $(./cost.sh $O/fl53 2>/dev/null)"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(bad)" = $W0 ] || die "a new kernel message"
say "ALL end: $(tail -1 $O/apply-end.txt); DONE"
