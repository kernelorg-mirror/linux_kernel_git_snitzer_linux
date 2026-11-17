#!/bin/bash
# write-check.sh <n>: the quick WRITE pass (Mike, 14 November).  Any
# change made for reads that touches a path WRITEs also take (the
# kernel's network, SUNRPC, nfsd, block or NVMe paths, xeu, or
# settled.sh) is checked with one two-port write run on all eight
# drives at the settled setting.  It passes at 77.6 GB/s or more with no
# drive stepped down and no new kernel warning; below 77.6 it is run once
# more (RERUN), and below 77.6 twice is a regression.  Until 16 November
# it passed at 78.0 and ran 77.8 to 77.99 again (Mike, 16 November:
# "77.8 to 77.99 aren't worth concern, anything before 77.6 should be
# flagged for re-run").
#
# It is short (Mike: it need only show that 78 GB/s is still reached):
# WARM=3 RUNTIME=15 and a 30 s rest, against the rounds' WARM=10
# RUNTIME=40 and 120 s.  In the 40 s runs t2-r631, r635, r643 and r644
# the wire is at full rate from its second second, and every 15 s
# stretch from the fourth on is within 0.2 GB/s of the whole run (wire
# 78.58 to 78.92, fio counting about 0.9% below the wire).  The drives
# need 20 s idle to be back in their fast region, and 15 s of 78 GB/s
# is about 150 GB a drive of the 625 they take at full rate.  WARM,
# RUNTIME and REST override.
#
# It keeps its files (twoport.sh's KEEP_FILES, Mike, 17 November): it
# writes the same files a read check fills, in place, so their markers
# stay and the read check after it reads them with no fill.
# KEEP_FILES=0 deletes them after.
cd "$(dirname "$0")" || exit 1
. ./common.sh
n=${1:?write-check.sh <run number>}
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(df --output=avail -k / | tail -1)" -gt 1572864 ] || { say "root has under 1.5 GB free"; exit 1; }
O=$L/t2-r$n; mkdir -p $O
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep ${REST:-30}
dmesg > $O/dmesg.0
env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
	WARM=${WARM:-3} RUNTIME=${RUNTIME:-15} RW=write KEEP_FILES=${KEEP_FILES:-1} RWTSV=$O/rw.tsv TAG=write-check \
	WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1 ||
	{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; exit 1; }
dmesg > $O/dmesg.1
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
gbs=$(grep '^total' $O/run.log | grep -oE '[0-9]+\.[0-9]+' | head -1)
down=$(grep -q 'no drive stepped down' $O/run.log && echo 0 || echo 1)
v=$(awk -v g="$gbs" -v d=$down -v h=$hung 'BEGIN {
	if (g == "" || d || h) print "FAIL"; else if (g >= 77.6) print "PASS";
	else print "RERUN" }')
say "t2-r$n write check: $v | $(grep '^total' $O/run.log) | $([ $down = 0 ] && echo 'no drive stepped down' || echo 'a drive stepped down') | new kernel warnings $hung"
grep -E '^  write:|^port |drives of' $O/run.log | sed 's/^/    /'
./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
[ $v = PASS ]
