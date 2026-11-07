#!/bin/bash
# run236.sh: round 63, the transport queue's dequeue a WRITE on build 514 at
# the client's defaults: one loan run on both ports, after a trim of the eight
# and 120 s of rest, deq.bt for 10 s inside the window (t2-r236).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
R=../FLAMEGRAPH/63-xprt-queue
say() { echo "$(date +%T) $*"; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
echo N > /sys/module/page_pool/parameters/rx_region_held_mark
O=$L/t2-r236; mkdir -p $O
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply-trace.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-trace.txt | head -3)"; exit 1; }
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
cat /proc/fs/nfsd/pool_stats > $O/pool_stats.0
( timed_load $O/trace 5 || exit 1
  timeout -s INT 10 bpftrace $R/deq.bt > $O/deq.txt 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=trace WRITES=128 ./twoport.sh $O/trace ab > $O/trace.log 2>&1 ||
	{ say "trace failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/trace.log | head -3)"; exit 1; }
wait
cat /proc/fs/nfsd/pool_stats > $O/pool_stats.1
say "t2-r236 trace | $(grep '^total' $O/trace.log) | $(grep -c . $O/deq.txt) lines"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
