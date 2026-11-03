#!/bin/bash
# run168.sh: the traces for rounds 55, 58 and 59 at the two-port setting (12 connections
# a port, 512 KiB, rings of 128): one loan run on both ports at its defaults, sglwalk.bt
# for 10 s inside the window, then regionlock.bt for 10 s (xeu-rig/).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r168
mkdir -p $O
say() { echo "$(date +%T) $*"; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), want_eighths $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_want_eighths)"
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
tag=walk
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply-$tag.txt 2>&1 || { say "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"; exit 1; }
sync
for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done
sleep 120
( timed_load $O/$tag 5 || exit 1
  timeout -s INT 10 bpftrace sglwalk.bt > $O/$tag-sglwalk.txt 2>&1
  timeout -s INT 10 bpftrace regionlock.bt > $O/$tag-regionlock.txt 2>&1 ) &
env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$tag WRITES=128 ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 ||
	{ say "$tag failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/$tag.log | head -3)"; exit 1; }
wait
say "$tag | $(grep '^total' $O/$tag.log) | $(grep traced $O/$tag-sglwalk.txt), $(grep traced $O/$tag-regionlock.txt)"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
