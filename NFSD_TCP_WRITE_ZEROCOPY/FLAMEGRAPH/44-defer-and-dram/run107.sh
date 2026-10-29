#!/bin/bash
# run107.sh: round 44's step 1, NAPI's deferral of the receive interrupts
# (A/B).  Round 41's kept xeu, build 499, rodata=noalias, the tip's sunrpc,
# nfsd the kernel's, the settled setting, region 32768.  Nine loan64 runs,
# counted and untraced (WARM=10 RUNTIME=30), port a's napi_defer_hard_irqs
# and gro_flush_timeout set after the settled setting is applied, in the
# order A B C C B A A B C:
#   A  0 / 0 ns       the settled setting: no deferral
#   B  2 / 10000 ns   a poll that completes keeps the ring masked and polls
#                     again from a 10 us timer, unmasking after 2 empty polls
#   C  2 / 30000 ns   the same with 30 us
# Each run also reports the RX interrupts a WRITE over 8 s from 10 s into
# its timed load (an open of the port, which twoport.sh makes, zeroes the
# counts in /proc/interrupts, so they are taken inside the load).  Both are
# put back to 0 at the end, and on a stop.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r107
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
PA=enp161s0f0 D=/sys/class/net/enp161s0f0
say() { echo "$(date +%T) $*"; }
defer() { echo $1 > $D/napi_defer_hard_irqs && echo $2 > $D/gro_flush_timeout; }
die() { say "STOPPED: $*"; defer 0 0; exit 1; }
rxirq() { awk -v n=$(nproc) '/enp161s0f0-Rx-/ {for (c = 2; c <= n + 1; c++) s += $c} END {print s}' /proc/interrupts; }
writes() { awk '/^proc3/ {print $10}' /proc/net/rpc/nfsd; }
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(awk '$1 == "pages" {print $2}' /sys/kernel/debug/page_pool_rx_region)" = 32768 ] || die "the region is not 32768 pages"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion)"
declare -A SET=([A]="0 0" [B]="2 10000" [C]="2 30000")
i=0
for a in A B C C B A A B C; do
	i=$((i + 1)); tag=r107-$i-loan64-$a
	load $tag
	defer ${SET[$a]} || die "$tag: deferral not set"
	( timed_load $O/$tag 10 || exit 1
	  q0=$(rxirq); w0=$(writes); sleep 8; q1=$(rxirq); w1=$(writes)
	  awk -v q=$((q1 - q0)) -v w=$((w1 - w0)) 'BEGIN {printf "%.2f\n", q / w}' > $O/$tag-irq.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait; set=$(cat $D/napi_defer_hard_irqs)/$(cat $D/gro_flush_timeout)
	defer 0 0
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | RX interrupts $(cat $O/$tag-irq.txt 2> /dev/null || echo -) a WRITE | defer $set"
done
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
say "end: defer $(cat $D/napi_defer_hard_irqs)/$(cat $D/gro_flush_timeout), $(tail -1 $O/apply-end.txt); DONE"
