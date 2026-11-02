#!/bin/bash
# run135.sh: round 52, verified writes through NFS.  During a light loan64 run,
# crc32c-verified O_DIRECT fio from the client, so that each length goes on the
# wire as it is, over NFSv3 and NFSv4.1, with padded.bt showing what arrived.
# run132, run133 and run134 wrote to /mnt/rxr-a0, a local directory: see README.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r132/nfs-$(cut -c1-6 /sys/module/sunrpc/srcversion); mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
client_free || exit 1
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
V="fio --ioengine=libaio --iodepth=4 --direct=1 --rw=write --size=256M --verify=crc32c --do_verify=1 --verify_fatal=1 --output-format=terse"
job() {	# job name dir bs: refuses a path that is not NFS
	$SSHC "t=\$(stat -f -c %T $2); case \$t in nfs*) ;; *) echo \"$2 is \$t, not NFS\"; exit 9 ;; esac; $V --name=$1 --bs=$3 --filename=$2/v-$1; r=\$?; rm -f $2/v-$1; exit \$r" > $O/$1.out 2>&1
	echo "$(date +%T) $1 bs=$3, fio rc=$? $(grep -m1 -E 'not NFS|err=|verify:' $O/$1.out | cut -c1-90)" >> $O/verify.txt
}
side() {
	timed_load $O/t 0 || exit 1
	timeout -s INT 50 bpftrace ../FLAMEGRAPH/52-bvec-walks/padded.bt > $O/padded.txt 2>&1 &
	for i in $(seq 60); do grep -q Attached $O/padded.txt && break; sleep 1; done
	echo "$(date +%T) probe attached" >> $O/verify.txt
	job v3-1m /mnt/rxr-a1 1048576
	job v3-aligned /mnt/rxr-a1 1048572
	for b in 1048573 1048574 1048575 524289 262147; do job v3-$b /mnt/rxr-a1 $b; done
	$SSHC "mkdir -p /mnt/rxr-a9 && mount -t nfs -o vers=4.1,proto=tcp,sec=sys,rsize=1048576,wsize=1048576 ${SRVP[a]}:/hs/nvme4/zcbudget /mnt/rxr-a9"; echo "v4.1 mount rc=$?" >> $O/verify.txt
	job v4-1m /mnt/rxr-a9 1048576
	job v4-1048573 /mnt/rxr-a9 1048573
	$SSHC "umount -f /mnt/rxr-a9"
	wait
}
: > $O/verify.txt
side &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 IODEPTH=1 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait
echo "rc=$rc $(grep '^total' $O/t.log) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/t/summary.txt)"; cat $O/verify.txt; grep -v Attached $O/padded.txt | sort -t: -k2 -n | tail -24
./settled.sh apply > $O/apply-end.txt 2>&1; tail -1 $O/apply-end.txt
