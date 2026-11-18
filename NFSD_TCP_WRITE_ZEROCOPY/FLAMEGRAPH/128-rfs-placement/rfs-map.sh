#!/bin/bash
# rfs-map.sh <n>: FLAMEGRAPH round 128, where RFS puts an NFS socket's
# TCP receive against where its work item reads the call (Mike, 18
# November: "look at the RFS placement, profile only").  One
# settled-iops.sh run of nfs4k.sh <n> and, from the load's eighth
# second, rfs-map.bt for 5 s; rfs-map.py reads its maps.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?rfs-map.sh <n>}
O=$L/t2-r$n; B=$(dirname $(readlink -f $0))
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	bpftrace $B/rfs-map.bt > $O/rfs-map.txt 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n; rc=$?
wait
python3 -I $B/rfs-map.py $O/rfs-map.txt
exit $rc
