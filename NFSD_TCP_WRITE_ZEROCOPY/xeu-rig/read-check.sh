#!/bin/bash
# read-check.sh <n>: the READ pass (Mike, 16 November), the twin of
# write-check.sh.  READs are locked at 73 GB/s with the client's rsize
# at 256 KiB (Mike, 16 November; 64 GB/s at rsize 1 MiB before): any
# change to a path READs take (the kernel's network, TCP, SUNRPC, nfsd,
# VFS, block or NVMe paths, xeu, or settled.sh) is checked with one
# two-port read run on all eight drives at the settled setting.  It
# passes at 73.0 GB/s or more with no new kernel warning; 72.5 to 72.99
# is run once more (RERUN); below 72.5, or below 73.0 twice, is a
# regression.
#
# The shape is round 104's read runs (run645.sh) without the profile:
# WARM=10 RUNTIME=40, the drives trimmed and a 120 s rest, but fio
# reads 1 MiB at a time (Mike, 16 November: 8 MiB was for the MD
# stripes, which are gone), the client's rsize 256 KiB, the drives'
# largest command (MDTS 6, max_hw_sectors_kb 256).  Build 534 read
# 63.50 at rsize 1 MiB (t2-r734), 67.59 at 512 KiB (r736) and 73.47 at
# 256 KiB (r737).  WARM, RUNTIME, REST, BS and RSIZE override.
cd "$(dirname "$0")" || exit 1
. ./common.sh
n=${1:?read-check.sh <run number>}
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(df --output=avail -k / | tail -1)" -gt 1572864 ] || { say "root has under 1.5 GB free"; exit 1; }
O=$L/t2-r$n; mkdir -p $O
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep ${REST:-120}
dmesg > $O/dmesg.0
env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
	WARM=${WARM:-10} RUNTIME=${RUNTIME:-40} RW=read BS=${BS:-1M} RSIZE=${RSIZE:-262144} RWTSV=$O/rw.tsv TAG=read-check \
	WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1 ||
	{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; exit 1; }
dmesg > $O/dmesg.1
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
gbs=$(grep '^total' $O/run.log | grep -oE '[0-9]+\.[0-9]+' | head -1)
v=$(awk -v g="$gbs" -v h=$hung 'BEGIN {
	if (g == "" || h) print "FAIL"; else if (g >= 73.0) print "PASS";
	else if (g >= 72.5) print "RERUN"; else print "FAIL" }')
say "t2-r$n read check: $v | $(grep '^total' $O/run.log) | new kernel warnings $hung"
grep -E '^  read:|^port |drives of' $O/run.log | sed 's/^/    /'
./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
[ $v = PASS ]
