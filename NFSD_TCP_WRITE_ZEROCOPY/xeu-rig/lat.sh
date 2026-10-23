#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# lat.sh - the low-depth latency yardstick of FLAMEGRAPH/README.md: what one
# loaned WRITE takes, with the drives far from saturation.
#
#   lat.sh OUTDIR TAG [WRITES]
#
# Two runs on port a and its four drives (/hs/nvme4-7), each after a trim and
# 60 s of rest: 4 writers, one a drive, iodepth 1, 1 MiB writes (so one NFS
# WRITE an I/O, four in flight), WARM=10 RUNTIME=30, WRITES=64 by default.
#   TAG-u   untraced: fio's completion latency at the client (p50, p99,
#           mean), in TAG-u.lat
#   TAG-t   lat.bt beside it: the server's split (TAG-t-lat.txt)
# Extra variables pass through to twoport.sh (SKB_DEFER_MAX= and the like).
cd "$(dirname "$(readlink -f "$0")")" || exit 1
. ./common.sh
O=${1:?OUTDIR}; tag=${2:?TAG}; WR=${3:-64}
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
run() {	# run TAG
	for n in 4 5 6 7; do fstrim /hs/nvme$n; done; sleep 60
	client_free || die "client not free"
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 JOBS=4 IODEPTH=1 BS=1M TSV=$O/results.tsv RWTSV=$O/lat.tsv TAG=$1 WRITES=$WR ./twoport.sh $O/$1 a > $O/$1.log 2>&1
}
lat() {	# lat TAG: fio's completion latency of the run, ms
	python3 -I - $O/$1/fio-a.json <<'EOF'
import json, sys
j = json.load(open(sys.argv[1])); w = j['jobs'][0]['write']; c = w['clat_ns']; p = c['percentile']
print("write %.2f GB/s, %.0f IOPS; completion ms: p50 %.3f, p90 %.3f, p99 %.3f, p99.9 %.3f, mean %.3f, max %.3f" % (
    w['bw_bytes'] / 1e9, w['iops'], p['50.000000'] / 1e6, p['90.000000'] / 1e6, p['99.000000'] / 1e6,
    p['99.900000'] / 1e6, c['mean'] / 1e6, c['max'] / 1e6))
EOF
}
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion)"
./settled.sh apply > $O/$tag-apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/$tag-apply.txt | head -3)"
run $tag-u || die "$tag-u: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag-u.log | head -3)"
lat $tag-u | tee $O/$tag-u.lat
bpftrace lat.bt > $O/$tag-t-lat.txt 2>&1 & bp=$!; sleep 5
run $tag-t; rc=$?
kill -INT $bp; wait $bp 2> /dev/null
[ $rc = 0 ] || die "$tag-t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag-t.log | head -3)"
lat $tag-t | tee $O/$tag-t.lat
./settled.sh apply > $O/$tag-apply-end.txt 2>&1
say "end: $(tail -1 $O/$tag-apply-end.txt); DONE"
