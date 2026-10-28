#!/bin/bash
# run94.sh: round 40's step 3, how long a woken nfsd thread takes to run.
# At the current state (build 499, rodata=noalias, the tip's sunrpc
# DDE22984B5905541BB2CD35, nfsd the kernel's, xeu round 35's, the settled
# setting), no module swapped, two runs on port a and its four drives:
#   r94-load   loan64 at full load (64 in flight)
#   r94-qd1    lat.sh's load: 4 writers, iodepth 1, 1 MiB (4 in flight)
# In each, from 10 s into the timed load (timed_load): 8 s of
# xeu-rig/rundelay.sh untraced, then 8 s of xeu-rig/wakelat.bt with
# rundelay.sh beside it, to see what the trace itself adds.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r94
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
run() {	# run TAG [env...]
	local t=$1 rc; shift
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	./settled.sh apply > $O/apply-$t.txt 2>&1 || die "$t not settled: $(grep -E 'BAD|FAILED' $O/apply-$t.txt | head -3)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	( timed_load $O/$t 10 || { echo "the timed load did not start" > $O/$t-look.when; exit 1; }
	  echo "untraced from $(date +%T)" > $O/$t-look.when
	  ./rundelay.sh 8 > $O/$t-rundelay-untraced.txt
	  echo "traced from $(date +%T)" >> $O/$t-look.when
	  ./rundelay.sh 8 > $O/$t-rundelay-traced.txt &
	  bpftrace wakelat.bt > $O/$t-wakelat.txt 2>&1
	  wait
	  echo "end $(date +%T)" >> $O/$t-look.when ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t "$@" ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
	say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(paste -sd ' ' $O/$t-look.when)"
	say "$t untraced: $(cat $O/$t-rundelay-untraced.txt)"
	say "$t traced:   $(cat $O/$t-rundelay-traced.txt)"
	say "$t wakelat:  $(grep -E '^@(total|schedule)_avg' $O/$t-wakelat.txt | paste -sd ' ')"
}
run r94-load WRITES=64
run r94-qd1 WRITES=64 JOBS=4 IODEPTH=1 BS=1M
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tail -1 $O/apply-end.txt); DONE"
