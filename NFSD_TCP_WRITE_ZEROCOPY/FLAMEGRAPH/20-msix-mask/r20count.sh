#!/bin/bash
# r20count.sh TAG: one loan64 run on port a at the settled setting with
# rearm.bt counting what re-arming the Rx interrupts costs, per WRITE.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r20; T=${1:?tag}
IRQS=$(grep -E "enp161s0f0-Rx" /proc/interrupts | awk '{print $1}' | tr -d : | paste -sd' ')
./settled.sh apply > $O/apply-$T.txt 2>&1 || { echo "not settled"; exit 1; }
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || { echo "client not free"; exit 1; }
bpftrace $O/rearm.bt $IRQS > $O/$T.rearm.txt 2>&1 & BP=$!
sleep 5
env FS_A="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$T WRITES=64 ./twoport.sh $O/$T a > $O/$T.log 2>&1; rc=$?
kill -INT $BP; wait $BP
w=$(grep -oE 'issued WRITE segments in the run: direct [0-9]+' $O/$T/window.txt | awk '{print $NF}')
echo "$T rc $rc | $(grep '^total' $O/$T.log) | WRITEs $w | $(grep -c 'no drive stepped down' $O/$T.log) no-step"
awk -v w=$w '/^@(hardirq|xeu_handler|enable|its_mask|its_unmask|its_retrigger|enable_ns_sum):/ {printf "  %-16s %12d  %.2f a WRITE\n", $1, $2, $2/w}' $O/$T.rearm.txt
grep -A16 '^@enable_ns:' $O/$T.rearm.txt
