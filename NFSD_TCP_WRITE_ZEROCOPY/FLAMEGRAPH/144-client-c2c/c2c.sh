#!/bin/bash
# c2c.sh <n> [iodepth] [seconds]: FLAMEGRAPH round 144, the client's
# cache misses by cache line (Mike, 19 November: "yes, run perf c2c on
# the client as round 144").  One settled-iops.sh run of nfs4k.sh
# <n>:<iodepth> (96) and, from the load's eighth second, <seconds> (5)
# of perf c2c record -a on the client (load and store latency samples),
# into its /dev/shm/zc-c2c-<n>.data, kept.  Into $L/t2-r<n>/: c2c.txt
# (perf c2c report --stdio, coalesced by instruction across threads,
# the first 4000 lines) and c2c-stats.txt.  Only reads the client.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?c2c.sh <n> [iodepth] [seconds]}; q=${2:-96}; s=${3:-5}
O=$L/t2-r$n; C=/dev/shm/zc-c2c-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf c2c record -a -o $C -- sleep $s > /dev/null 2>&1"
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
$SSHC "perf c2c report -i $C --stdio -c iaddr --full-symbols 2>/dev/null | head -4000" > $O/c2c.txt
$SSHC "perf c2c report -i $C --stdio --stats 2>/dev/null" > $O/c2c-stats.txt
exit $rc
