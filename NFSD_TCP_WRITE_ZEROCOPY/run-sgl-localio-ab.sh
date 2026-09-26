#!/bin/bash
# run-sgl-localio-ab.sh tcp|loop OUTDIR -- LOCALIO boundary, short-read and
# stability tests on XFS over an nvmet FILE-backed (tmpfs image) namespace,
# transport tcp (virt boundary 0) or loop (4096) -- the start-nfsd.sh rig shape.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
STATX=$HERE/statx-dio; [ -x $STATX ] || gcc -O2 -Wall -o $STATX $HERE/statx-dio.c || exit 2
TR=$1; OUT=$2; L=$HERE/../NFS_LOCALIO_DONTCACHE; C=/sys/kernel/config/nvmet; T=/sys/kernel/tracing
NQN=sgl-file-$TR; IMG=/dev/shm/$NQN.img; P=9
truncate -s 4G $IMG
modprobe nvmet; modprobe nvmet-tcp; modprobe nvme-tcp; modprobe nvme-loop
mkdir -p $C/subsystems/$NQN/namespaces/1 $C/ports/$P
echo 1 > $C/subsystems/$NQN/attr_allow_any_host
echo -n $IMG > $C/subsystems/$NQN/namespaces/1/device_path
echo 1 > $C/subsystems/$NQN/namespaces/1/enable
if [ $TR = tcp ]; then echo tcp > $C/ports/$P/addr_trtype; echo ipv4 > $C/ports/$P/addr_adrfam; echo 127.0.0.1 > $C/ports/$P/addr_traddr; echo 4421 > $C/ports/$P/addr_trsvcid
else echo loop > $C/ports/$P/addr_trtype; fi
ln -s $C/subsystems/$NQN $C/ports/$P/subsystems/$NQN
if [ $TR = tcp ]; then nvme connect -t tcp -a 127.0.0.1 -s 4421 -n $NQN >/dev/null; else nvme connect -t loop -n $NQN >/dev/null; fi
sleep 1
D=$(for c in /sys/class/nvme/nvme*; do [ "$(cat $c/subsysnqn 2>/dev/null)" = $NQN ] && ls -d /dev/$(basename $c)n1; done)
[ -b "$D" ] || { echo "no device"; exit 1; }
mkdir -p /export/t4; mkfs.xfs -f -q $D && mount $D /export/t4 && exportfs -o rw,no_root_squash,insecure,no_subtree_check,fsid=4700 '*:/export/t4'
: > /export/t4/probe; echo "$TR $D: vbm=$(cat /sys/block/$(basename $D)/queue/virt_boundary_mask) $($STATX /export/t4/probe | grep -oE 'dio_mem_align=[0-9]+|dio_seg_boundary=[0-9]+' | tr '\n' ' ')"; rm -f /export/t4/probe
echo 0 > $T/tracing_on; echo > $T/trace; for e in nfsd/nfsd_file_get_dio_attrs nfsd/nfsd_write_dio_split; do echo 1 > $T/events/$e/enable; done; echo 1 > $T/tracing_on
echo Y > /sys/module/nfs/parameters/localio_enabled
$HERE/../start-nfsd.sh mount-client -L -v 3 2>&1 | tail -1
$L/localio-boundary-test.sh -l $TR $OUT/boundary > $OUT.boundary.out 2>&1; echo "boundary exit=$?"; grep '^rs=' $OUT/boundary/summary.txt
$L/localio-short-read-test.sh $OUT/short > $OUT.short.out 2>&1; echo "short-read exit=$?"
$L/localio-stable-test.sh $OUT/stable > $OUT.stable.out 2>&1; echo "stable exit=$?"
echo 0 > $T/tracing_on
echo "policy seen: $(grep nfsd_file_get_dio_attrs $T/trace | grep -oE 'seg_boundary=-?[0-9]+ joint_boundary=[0-9]+' | sort | uniq -c | tr '\n' ' ') rpc_writes=$(grep -c nfsd_write_dio_split: $T/trace)"
umount /mnt/t4; exportfs -u '*:/export/t4'; umount /export/t4
nvme disconnect -n $NQN >/dev/null; rm -f $C/ports/$P/subsystems/$NQN; echo 0 > $C/subsystems/$NQN/namespaces/1/enable; rmdir $C/subsystems/$NQN/namespaces/1 $C/subsystems/$NQN $C/ports/$P; rm -f $IMG
