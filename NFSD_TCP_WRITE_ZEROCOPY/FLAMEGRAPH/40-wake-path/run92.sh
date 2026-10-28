#!/bin/bash
# run92.sh: round 40's step 1, where nfsd's wakes come from and what each
# lands on.  At the current state (build 499, rodata=noalias, the tip's
# sunrpc DDE22984B5905541BB2CD35, nfsd the kernel's, xeu round 35's, the
# settled setting), no module swapped: one untraced loan64 run, counted, and
# one traced loan64 run with, from 10 s into its timed load (timed_load),
# 8 s of xeu-rig/wakesrc.bt (the wakes by source and the passes through
# svc_recv, as round 17's) and then 8 s of xeu-rig/wakeland.bt (where a
# woken thread runs, how long to get there, and every switch on nfsd's CPUs).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r92
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XB"
W0=$(dmesg | grep -c 'WARNING:')
load() {	# load TAG: the settled setting, checked, the drives trimmed and rested
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
line() {
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg"
	say "$1 | $(grep '^total' $O/$1.log) | $(grep -c 'no drive stepped down' $O/$1.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$1/geometry.txt) placed | $(./cost.sh $O/$1) | peak $((32768 - $(sort -n $O/$1/region-min-free.txt | head -1))) | $(grep '^nfsd in the window' $O/$1/summary.txt)"
}

t=r92-1-loan64
load $t
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1 || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
line $t

t=r92-trace-loan64
load $t
( timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "wakesrc from $(date +%T)" > $O/look.when
  bpftrace wakesrc.bt > $O/wakesrc.txt 2>&1
  echo "wakeland from $(date +%T)" >> $O/look.when
  bpftrace wakeland.bt > $O/wakeland.txt 2>&1
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
line $t
say "$(paste -sd ' ' $O/look.when)"
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tail -1 $O/apply-end.txt); DONE"
