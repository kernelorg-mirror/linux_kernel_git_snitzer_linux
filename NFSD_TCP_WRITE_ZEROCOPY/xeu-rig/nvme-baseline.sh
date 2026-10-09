#!/bin/bash
# nvme-baseline.sh <prp|sgl> [outdir] - what the storage of the step cells
# takes from fio run on tardis1 itself, with nothing of NFS or the network in
# the way: the ceiling for the cells of that storage.
#
#   prp  a scratch directory in /hs/nvme0 (the native drive, needs whole pages)
#   sgl  an NVMe/TCP target over loopback on a 4 GiB file in /hs/nvme1, with
#        XFS on it, made as run-budget.sh makes its own (its own NQN and port)
#
# Three jobs, all 1 MiB direct writes at iodepth 16:
#   step     the step cell's own job (one 1 GiB file overwritten for 120 s,
#            verify_backlog reading back as it goes)
#   write    the same without the verify: writes alone, and the rate each
#            10 s, which shows a drive that slows as it fills its cache
#   write4   four such files at once for 60 s
# Run it only when no sweep is running.  Everything it makes it removes.
set -u
ST=${1:?prp or sgl}; . "$(dirname "$0")/common.sh"
OUT=${2:-$L/xeu-$(( PS / 1024 ))k-nvme-baseline}; mkdir -p $OUT
CFG=/sys/kernel/config; NQN=zc-baseline; TPORT=4478
# PRP_BASE and SGL_BASE: the mounted file systems to measure (a stripe:
# PRP_BASE=/hs/stripe SGL_BASE=/hs/stripe).  JOBS: the writers of the last job.
PRP_BASE=${PRP_BASE:-/hs/nvme0}; SGL_BASE=${SGL_BASE:-/hs/nvme1}; JOBS=${JOBS:-4}
IMG=$SGL_BASE/zcbaseline-nvmet.img; MNT=/mnt/zcbaseline
pgrep -x sweep.sh > /dev/null && { echo "a sweep is running"; exit 1; }

teardown() {
	if [ $ST = sgl ]; then
		mountpoint -q $MNT && umount $MNT
		nvme disconnect -n $NQN > /dev/null 2>&1
		for l in $CFG/nvmet/ports/*/subsystems/$NQN; do
			[ -L "$l" ] || continue
			rm -f "$l"; rmdir "$(dirname "$(dirname "$l")")" 2>/dev/null
		done
		[ -d $CFG/nvmet/subsystems/$NQN/namespaces/1 ] && { echo 0 > $CFG/nvmet/subsystems/$NQN/namespaces/1/enable; rmdir $CFG/nvmet/subsystems/$NQN/namespaces/1; }
		rmdir $CFG/nvmet/subsystems/$NQN 2>/dev/null
		rm -f $IMG; rmdir $MNT 2>/dev/null
	else
		rm -rf $PRP_BASE/zcbaseline
	fi
}
trap teardown EXIT

if [ $ST = sgl ]; then
	modprobe nvmet; modprobe nvmet-tcp; modprobe nvme-tcp
	dd if=/dev/zero of=$IMG bs=1M count=${IMG_MIB:-4096} oflag=direct status=none || exit 1
	mkdir $CFG/nvmet/subsystems/$NQN || exit 1
	echo 1 > $CFG/nvmet/subsystems/$NQN/attr_allow_any_host
	mkdir $CFG/nvmet/subsystems/$NQN/namespaces/1
	echo $IMG > $CFG/nvmet/subsystems/$NQN/namespaces/1/device_path
	echo 1 > $CFG/nvmet/subsystems/$NQN/namespaces/1/enable
	for P in $(seq 7 64); do [ -e $CFG/nvmet/ports/$P ] || break; done
	mkdir $CFG/nvmet/ports/$P
	echo tcp > $CFG/nvmet/ports/$P/addr_trtype
	echo ipv4 > $CFG/nvmet/ports/$P/addr_adrfam
	echo 127.0.0.1 > $CFG/nvmet/ports/$P/addr_traddr
	echo $TPORT > $CFG/nvmet/ports/$P/addr_trsvcid
	ln -s $CFG/nvmet/subsystems/$NQN $CFG/nvmet/ports/$P/subsystems/$NQN
	nvme connect -t tcp -a 127.0.0.1 -s $TPORT -n $NQN > /dev/null || exit 1
	dev=
	for _ in $(seq 50); do
		for c in /sys/class/nvme/nvme*; do
			[ "$(cat $c/subsysnqn 2>/dev/null)" = $NQN ] && dev=/dev/$(basename $c)n1
		done
		[ -b "$dev" ] && break; sleep 0.1
	done
	[ -b "$dev" ] || { echo "no namespace for $NQN"; exit 1; }
	mkfs.xfs -q -f -K $dev && mkdir -p $MNT && mount $dev $MNT || exit 1
	DIR=$MNT; SZ4=512M
else
	dev=$(findmnt -n -o SOURCE --target $PRP_BASE); DIR=$PRP_BASE/zcbaseline; mkdir -p $DIR; SZ4=1G
fi
echo "$ST: $dev virt_boundary_mask $(cat /sys/block/$(basename $dev)/queue/virt_boundary_mask), files in $DIR, page $PS, $(uname -r)" | tee $OUT/$ST.rig

COMMON="--rw=write --bs=1M --direct=1 --ioengine=libaio --iodepth=16 --time_based --output-format=json"
s0=$(head -1 /proc/stat)
fio --name=step --filename=$DIR/f-step --size=1G --runtime=120 --verify=crc32c --verify_backlog=64 --do_verify=1 $COMMON > $OUT/$ST.step.json
fio --name=write --filename=$DIR/f-write --size=1G --runtime=120 --write_bw_log=$OUT/$ST.write --log_avg_msec=10000 $COMMON > $OUT/$ST.write.json
rm -f $DIR/f-step $DIR/f-write	# the 4 GiB target has no room for them and four more
fio --name=write4 --directory=$DIR --size=$SZ4 --numjobs=$JOBS --group_reporting --runtime=60 $COMMON > $OUT/$ST.write4.json
for j in step write write4; do
	python3 -c "
import json,sys
s=open('$OUT/$ST.$j.json').read()
w=json.loads(s[s.index('{'):])['jobs'][0]['write']
print('$ST %-6s %6.0f MB/s  mean latency %.2f ms' % ('$j', w['bw_bytes']/1e6, w['lat_ns']['mean']/1e6))"
done | tee $OUT/$ST.txt
echo "$ST write, MB/s each 10 s: $(awk -F, '{printf "%.0f ", $2*1024/1e6}' $OUT/$ST.write_bw.1.log)" | tee -a $OUT/$ST.txt
