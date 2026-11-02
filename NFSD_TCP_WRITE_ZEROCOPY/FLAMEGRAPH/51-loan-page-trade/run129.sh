#!/bin/bash
# run129.sh: round 51, the trace behind the fix: pagetrade.bt for 8 s in one
# loan64 run's window.  A look, so its figures are not the run's.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r129; mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
client_free || exit 1
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60
( timed_load $O/t 10 && timeout 30 bpftrace ../FLAMEGRAPH/51-loan-page-trade/pagetrade.bt > $O/pagetrade.txt 2>&1 ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait
echo "rc=$rc $(grep '^total' $O/t.log) WRITEs/s $(grep -o 'WRITEs [0-9]*' $O/t/window.txt | head -1)"
./settled.sh apply > $O/apply-end.txt 2>&1; tail -1 $O/apply-end.txt
