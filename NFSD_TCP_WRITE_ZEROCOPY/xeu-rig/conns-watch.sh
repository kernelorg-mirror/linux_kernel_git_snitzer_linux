#!/bin/bash
# conns-watch.sh <mounts> <outdir>: one best-setting cell with
# <mounts> mounts of nconnect 16, watched: 30 s into the cell the wire
# rate and the client's CPUs are read, and if the port carries under
# 25 GB/s the cell is stopped (the client fell over at two mounts on
# 2026-10-10 before its reboot).
M=$1; OUT=$2
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig
. ./common.sh
STEP_MOUNTS=$M RX_LINGER_US=50 PLACE=1 PRP_DIR=/hs/stripe/zcbudget TRIM=/hs/stripe STEP_JOBS=16 STEP_BS=8M ./trace-sweep.sh $OUT plans/16k-32-best.plan &
SW=$!
until grep -qs ' step-copy-n16 fio start' $OUT/r*/stress-A/delay-steps.txt; do kill -0 $SW 2>/dev/null || { echo "sweep ended before the cell started"; exit 1; }; sleep 2; done
sleep 30
$SSHC 'mpstat 5 1 | tail -1' > $OUT/watch-client-mpstat.txt 2>&1 &
r=$(./rings.sh 5 | awk '$1 == "all" {print $2}')
wait %2 2>/dev/null
echo "watch: 30 s in the port carries $r GB/s; client: $(cat $OUT/watch-client-mpstat.txt)" | tee $OUT/watch.txt
if awk -v r="$r" 'BEGIN {exit !(r < 25)}'; then
	echo "watch: under 25 GB/s: stopping the cell" | tee -a $OUT/watch.txt
	kill $SW; for p in $(pgrep -x -f "/bin/bash ./sweep.sh $OUT plans/16k-32-best.plan"); do kill $p; done
	for p in $(ps -eo pid,args | awk -v o="$OUT" '$3 == "./run-budget.sh" && index($4, o) == 1 {print $1}'); do kill -TERM $p; done
	sleep 3
	$SSHC 'pkill -x fio; sleep 2; for m in /mnt/rxr /mnt/rxr.*; do mountpoint -q $m && umount $m; done; true'
	rm -f /hs/stripe/zcbudget/f-step-copy-n16.* /hs/stripe/zcbudget/bal-*
	# The cell's further addresses are taken off by the cell itself, which was killed.
	for i in $(seq 1 $(( M - 1 ))); do ip addr del 192.168.1.$(( 211 + i ))/24 dev $D 2>/dev/null; done
	echo WATCHSTOPPED
	exit 2
fi
wait $SW
echo WATCHDONE
