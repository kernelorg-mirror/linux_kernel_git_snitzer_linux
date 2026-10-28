#!/bin/bash
# run80.sh: round 37, one profiled loan64 run at the current state after
# run79.sh's four counted ones, on the same boot (build 499, rodata=noalias,
# the tip's sunrpc E80412A536ADD1E86B16F5A, nfsd the kernel's, xeu round
# 35's, the settled setting): a fresh profile for round 38 to start from.
# twoport.sh's PROFILE=1 (perf stat of the rings' CPUs and nfsd's, then
# where the cycles and the L3 misses go), which needs RUNTIME=55; the perf
# data is kept.  Its counts are taken while perf samples, so it is not part
# of run79.sh's mean.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r80
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=E80412A536ADD1E86B16F5A
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XB"
W0=$(dmesg | grep -c 'WARNING:')
tag=r80-profile-loan64-now
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply-$tag.txt 2>&1 || die "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
env FS_A="$FS" FS_B= WARM=10 RUNTIME=55 PROFILE=1 KEEP_PERF=1 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$tag: a new WARNING in dmesg"
say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tail -1 $O/apply-end.txt); DONE"
