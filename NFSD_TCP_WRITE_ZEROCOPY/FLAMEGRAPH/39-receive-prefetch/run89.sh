#!/bin/bash
# run89.sh: round 39's start, the tip counted and profiled.  The tip's
# sunrpc, DDE22984B5905541BB2CD35 (built from 93b10c1877fc, "SUNRPC: Fetch
# a released page's struct page a second time", in t2-r39-tip), takes the
# place of round 36's E80412A536ADD1E86B16F5A and stays: it is the kept
# sunrpc from here.  Then, at the current state (build 499, rodata=noalias,
# nfsd the kernel's, xeu round 35's, the settled setting), four untraced
# loan64 runs counted as run79.sh's, and one with twoport.sh's PROFILE=1 as
# run80.sh's (RUNTIME=55, the perf data kept).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r89
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
if [ "$(cat /sys/module/sunrpc/srcversion)" != $K ]; then
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	./swap-sunrpc.sh $L/t2-r39-tip > $O/swap-t2-r39-tip.txt 2>&1 || die "the tip's sunrpc did not go in: $(tail -3 $O/swap-t2-r39-tip.txt)"
fi
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is $(cat /sys/module/sunrpc/srcversion), not $K"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XB"
W0=$(dmesg | grep -c 'WARNING:')
run() {	# run TAG [env...]: one loan64 run, settled first
	local tag=$1 rc; shift
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || die "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	env FS_A="$FS" FS_B= WARM=10 TSV=$O/results.tsv TAG=$tag WRITES=64 "$@" ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$tag: a new WARNING in dmesg"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
}
for i in 1 2 3 4; do run r89-$i-loan64-tip RUNTIME=30; done
run r89-profile-loan64-tip RUNTIME=55 PROFILE=1 KEEP_PERF=1
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion), $(tail -1 $O/apply-end.txt); DONE"
