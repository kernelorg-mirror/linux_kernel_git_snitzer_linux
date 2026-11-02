#!/bin/bash
# run132.sh: round 52, does what a client writes come back: during a saturating
# loan64 run on B, crc32c-verified fio from the client on the run's own export,
# over NFSv3 at 1 MiB, NFSv3 at a length that needs a pad, and NFSv4.1 at 1 MiB.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r132/verify; mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
client_free || exit 1
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60
V="fio --ioengine=libaio --iodepth=8 --direct=1 --rw=write --size=1G --verify=crc32c --do_verify=1 --verify_fatal=1 --output-format=terse"
side() {
	timed_load $O/t 8 || exit 1
	$SSHC "$V --name=v3-1m --bs=1048576 --filename=/mnt/rxr-a0/f-v3-1m" > $O/v3-1m.out 2>&1; echo "v3-1m rc=$?" >> $O/verify.txt
	$SSHC "$V --name=v3-pad --bs=1048573 --filename=/mnt/rxr-a0/f-v3-pad" > $O/v3-pad.out 2>&1; echo "v3-pad rc=$?" >> $O/verify.txt
	$SSHC "mkdir -p /mnt/rxr-a9 && mount -t nfs -o vers=4.1,proto=tcp,sec=sys,rsize=1048576,wsize=1048576 ${SRVP[a]}:/hs/nvme4/zcbudget /mnt/rxr-a9 && $V --name=v4-1m --bs=1048576 --filename=/mnt/rxr-a9/f-v4-1m; r=\$?; umount -f /mnt/rxr-a9; exit \$r" > $O/v4-1m.out 2>&1; echo "v4-1m rc=$?" >> $O/verify.txt
}
: > $O/verify.txt
side &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait
echo "rc=$rc $(grep '^total' $O/t.log) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/t/summary.txt)"; cat $O/verify.txt
./settled.sh apply > $O/apply-end.txt 2>&1; tail -1 $O/apply-end.txt
