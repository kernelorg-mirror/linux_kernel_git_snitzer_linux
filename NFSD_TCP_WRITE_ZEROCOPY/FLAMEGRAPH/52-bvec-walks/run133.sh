#!/bin/bash
# run133.sh: round 52, the lengths that need a pad, on whichever sunrpc is in:
# during a saturating loan64 run, an odd direct write, and odd buffered files
# written, dropped from the client's cache and compared.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r132/verify-$(cat /sys/module/sunrpc/srcversion | cut -c1-6); mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
client_free || exit 1
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60
V="fio --ioengine=libaio --iodepth=8 --direct=1 --rw=write --size=1G --verify=crc32c --do_verify=1 --verify_fatal=1 --output-format=terse"
side() {
	timed_load $O/t 8 || exit 1
	$SSHC "$V --name=v3-pad --bs=1048573 --filename=/mnt/rxr-a0/f-v3-pad" > $O/v3-pad.out 2>&1; echo "v3-pad direct, fio rc=$?" >> $O/verify.txt
	$SSHC "dd if=/dev/zero of=/mnt/rxr-a0/f-dd bs=1048573 count=1 oflag=direct" > $O/dd-direct.out 2>&1; echo "odd length direct, dd rc=$?: $(tail -1 $O/dd-direct.out)" >> $O/verify.txt
	for n in 1000001 1000002 1000003 4099; do
		$SSHC "dd if=/dev/urandom of=/var/tmp/p$n bs=$n count=1 iflag=fullblock status=none && cp /var/tmp/p$n /mnt/rxr-a0/f-p$n && sync /mnt/rxr-a0/f-p$n && echo 3 > /proc/sys/vm/drop_caches && cmp /var/tmp/p$n /mnt/rxr-a0/f-p$n; r=\$?; rm -f /var/tmp/p$n; exit \$r" > $O/p$n.out 2>&1
		echo "buffered $n, cmp rc=$?" >> $O/verify.txt
	done
}
: > $O/verify.txt
side &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait
echo "rc=$rc $(grep '^total' $O/t.log) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/t/summary.txt)"; cat $O/verify.txt
./settled.sh apply > $O/apply-end.txt 2>&1; tail -1 $O/apply-end.txt
