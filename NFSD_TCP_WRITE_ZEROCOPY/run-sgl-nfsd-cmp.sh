#!/bin/bash
# run-sgl-nfsd-cmp.sh RIG CONNS (rigs from run-sgl-rigs.sh up) - NFS WRITE path over one SGL rig (brd|tcp|loop):
# fresh XFS, loans on, io_cache_write=2, LOCALIO off, CONNS fresh v4.2
# connections x 16 MiB O_DIRECT, cmp each file; reports dispositions.
set -u
R=$1; N=${2:-10}; E=/export/sgl-$R; M=/mnt/sgl-$R; T=/sys/kernel/tracing
SRC=/dev/shm/sgl-src-16m; [ -f $SRC ] || dd if=/dev/urandom of=$SRC bs=1M count=16 status=none
case $R in brd) D=/dev/ram0;; tcp|loop) D=$(for c in /sys/class/nvme/nvme*; do [ "$(cat $c/subsysnqn 2>/dev/null)" = sgl-$R ] && ls -d /dev/$(basename $c)n1; done);; esac
[ -b "$D" ] || { echo "no device for $R"; exit 1; }
mountpoint -q $E && { exportfs -u "*:$E"; umount $E; }
mkfs.xfs -f -q $D && mount $D $E || exit 1
i=0; for r in brd tcp loop; do i=$((i+1)); [ $r = $R ] && exportfs -o rw,no_root_squash,insecure,fsid=88$i "*:$E"; done
echo 2 > /sys/kernel/debug/nfsd/io_cache_write
echo N > /sys/module/nfs/parameters/localio_enabled
echo Y > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages
echo 0 > $T/tracing_on; echo > $T/trace
for e in nfsd/nfsd_write_dio_split nfsd/nfsd_write_direct nfsd/nfsd_write_vector sunrpc/svcsock_tcp_rx_lifetime nfsd/nfsd_file_get_dio_attrs; do echo 1 > $T/events/$e/enable; done
echo 1 > $T/tracing_on
fails=0; mism=0
for i in $(seq $N); do
	mount -t nfs -o vers=4.2,proto=tcp,sec=sys 127.0.0.1:$E $M || { fails=$((fails+1)); continue; }
	dd if=$SRC of=$M/f$i bs=1M count=16 oflag=direct conv=fsync status=none || fails=$((fails+1))
	umount $M
	cmp -s $SRC $E/f$i || mism=$((mism+1))
done
echo 0 > $T/tracing_on
for e in nfsd/nfsd_write_dio_split nfsd/nfsd_write_direct nfsd/nfsd_write_vector sunrpc/svcsock_tcp_rx_lifetime nfsd/nfsd_file_get_dio_attrs; do echo 0 > $T/events/$e/enable; done
echo "rig=$R dev=$D conns=$N dd_fails=$fails cmp_mismatches=$mism"
grep nfsd_file_get_dio_attrs $T/trace | grep -oE 'mem_align=[0-9]+|seg_boundary=-?[0-9]+|joint_boundary=[0-9]+' | sort | uniq -c | tr '\n' ' '; echo
echo "dispositions: $(grep -oE 'disposition=[a-z_]+' $T/trace | sort | uniq -c | tr '\n' ' ')  direct=$(grep -c nfsd_write_direct: $T/trace) vector=$(grep -c nfsd_write_vector: $T/trace)"
echo "loans: $(grep 'action=publish' $T/trace | awk '{for(i=1;i<=NF;i++) if($i~/^(borrowed|copied)=/){split($i,a,"=");s[a[1]]+=a[2]}} END{printf "borrowed=%d copied=%d", s["borrowed"], s["copied"]}') publishes=$(grep -c 'action=publish' $T/trace) reasons: $(grep 'action=publish' $T/trace | grep -oE 'reason=[a-z-]+' | sort | uniq -c | tr '\n' ' ')"
