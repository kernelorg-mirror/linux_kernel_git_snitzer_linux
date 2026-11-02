#!/bin/bash
# run140.sh: round 54, fh.bt again, without the kretprobes that spoiled run139's
# counts.  run139's perf record, of cycles, instructions and L2 refills on nfsd's
# CPUs with no call chains and no probes, stands.  A look.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r140; mkdir -p $O
R=../FLAMEGRAPH/54-fh-verify
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
prep() {
	client_free || exit 1
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
	./settled.sh apply > $O/apply-$1.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply-$1.txt | head; exit 1; }
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 90
}
load() { env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TAG=$1 WRITES=64 ./twoport.sh $O/$1 a > $O/$1.log 2>&1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { echo "kdump is not armed"; exit 1; }
prep t
( timed_load $O/t 10 && timeout 40 bpftrace $R/fh.bt > $O/fh.txt 2>&1 ) &
load t; rc=$?; wait
echo "$(date +%T) trace run rc=$rc: $(grep '^total' $O/t.log | head -1)"
./settled.sh apply > $O/apply-end.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply-end.txt | head; exit 1; }
echo "$(date +%T) end: $(./settled.sh audit rest 2>&1 | tail -1); warnings $(dmesg | grep -cE 'WARNING:|BUG:|Oops')"
