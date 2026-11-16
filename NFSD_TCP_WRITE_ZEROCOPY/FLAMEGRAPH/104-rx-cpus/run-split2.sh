#!/bin/bash
# run-split2.sh <n>: round 104, svc_rtc_tx_split on both ports at the
# two-port setting (Mike, 16 November), on the module with the teardown's
# fixes.  The run is read-check.sh's (all eight drives, fio 1 MiB at rsize
# 256 KiB, WARM=10 RUNTIME=40 after a trim and 120 s of rest) with split Y,
# so its figure stands beside the read mark.  t2-r759 hung the host on two
# ports, on the module before the RXOWN fix: this refuses to run off a boot
# with pseudo-NMI and kdump, and split goes back to N whatever happens.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
n=${1:?run number}
P=/sys/module/sunrpc/parameters
grep -qw 'irqchip.gicv3_pseudo_nmi=1' /proc/cmdline || { say "no pseudo-NMI on this boot"; exit 1; }
[ "$(sysctl -n kernel.nmi_watchdog)" = 1 ] || { say "NMI watchdog off"; exit 1; }
[ "$(sysctl -n kernel.hardlockup_panic)" = 1 ] || { say "hardlockup_panic off"; exit 1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = 8843BD2AFB934E2FEFD942B ] || { say "not the split sunrpc"; exit 1; }
O=$L/t2-r$n; mkdir -p $O || exit 1
client_free || { say "client not free"; exit 1; }
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
sync; [ "${TRIM:-1}" = 0 ] || for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep ${REST:-120}
dmesg > $O/dmesg.0
trap 'echo N > $P/svc_rtc_tx_split' EXIT
echo Y > $P/svc_rtc_tx_split	# latched by each connection at attach: the mounts below
say "t2-r$n: two ports, split $(cat $P/svc_rtc_tx_split)"
cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.0 2>/dev/null
env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
	WARM=${WARM:-10} RUNTIME=${RUNTIME:-40} RW=read BS=${BS:-1M} RSIZE=${RSIZE:-262144} RWTSV=$O/rw.tsv TAG=split2-$n \
	WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1
rc=$?
cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.1 2>/dev/null
echo N > $P/svc_rtc_tx_split
dmesg > $O/dmesg.1
hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|svc_rtc:')
say "twoport exit $rc: $(grep '^total' $O/run.log) | new kernel warnings $hung"
grep -E '^  read:|^port |drives of' $O/run.log | sed 's/^/    /'
./settled.sh apply > $O/apply-after.txt 2>&1; say "after: $(tail -1 $O/apply-after.txt)"
exit $rc
