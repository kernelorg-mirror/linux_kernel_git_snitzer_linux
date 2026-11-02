#!/bin/bash
# run134.sh: round 52, lengths that need a pad, shown to arrive loaned: during a
# light loan64 run, files of odd length written with one WRITE each, dropped
# from the client's cache and compared, with padded.bt counting them.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r132/pad-$(cut -c1-6 /sys/module/sunrpc/srcversion); mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
client_free || exit 1
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
side() {
	timed_load $O/t 6 || exit 1
	timeout -s INT 22 bpftrace ../FLAMEGRAPH/52-bvec-walks/padded.bt > $O/padded.txt 2>&1 &
	sleep 4
	for n in 1000001 1000002 1000003 1048573 1048575 524289 262147 4099; do
		$SSHC "dd if=/dev/urandom of=/var/tmp/p$n bs=$n count=1 iflag=fullblock status=none && cp /var/tmp/p$n /mnt/rxr-a0/v-p$n && sync /mnt/rxr-a0/v-p$n && echo 3 > /proc/sys/vm/drop_caches && cmp /var/tmp/p$n /mnt/rxr-a0/v-p$n; r=\$?; rm -f /var/tmp/p$n /mnt/rxr-a0/v-p$n; exit \$r" > $O/p$n.out 2>&1
		echo "$(date +%T) buffered $n, cmp rc=$? $(cut -c1-80 $O/p$n.out)" >> $O/verify.txt
	done
	$SSHC "dd if=/dev/zero of=/mnt/rxr-a0/v-dd bs=1048573 count=1 oflag=direct; rm -f /mnt/rxr-a0/v-dd" > $O/dd-direct.out 2>&1
	echo "odd direct, dd rc=$?: $(grep -m1 error $O/dd-direct.out | cut -c1-70)" >> $O/verify.txt
	wait
}
: > $O/verify.txt
side &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=25 IODEPTH=1 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait
echo "rc=$rc $(grep '^total' $O/t.log) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/t/summary.txt)"; cat $O/verify.txt; grep -v Attached $O/padded.txt
./settled.sh apply > $O/apply-end.txt 2>&1; tail -1 $O/apply-end.txt
