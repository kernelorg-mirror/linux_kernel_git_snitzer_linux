#!/bin/bash
# run-split1.sh <n>: round 104, svc_rtc_tx_split again after t2-r759 hung
# tardis2, on one port (a, /hs/nvme0 to 3) and four connections, on a boot
# with pseudo-NMI so that a hard lockup panics into kdump.  A short read at
# rsize 256 KiB with split Y; split is put back to N after it, whatever
# happens.  Refuses to run unless the boot is the one it is meant for.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
n=${1:?run number}
P=/sys/module/sunrpc/parameters
grep -qw 'irqchip.gicv3_pseudo_nmi=1' /proc/cmdline || { say "no pseudo-NMI on this boot"; exit 1; }
[ "$(sysctl -n kernel.nmi_watchdog)" = 1 ] || { say "NMI watchdog off"; exit 1; }
[ "$(sysctl -n kernel.hardlockup_panic)" = 1 ] || { say "hardlockup_panic off"; exit 1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = 1907D42B9BE624B41C23DA2 ] || { say "not the split sunrpc"; exit 1; }
[ -e $P/svc_rtc_tx_split ] || { say "no svc_rtc_tx_split"; exit 1; }
O=$L/t2-r$n; mkdir -p $O || exit 1
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
ping -c1 -W2 192.168.1.106 > /dev/null || { say "port a does not answer"; exit 1; }
dmesg > $O/dmesg.0
trap 'echo N > $P/svc_rtc_tx_split' EXIT
sync; sleep ${REST:-10}
echo Y > $P/svc_rtc_tx_split	# latched by each connection at attach: the mounts below
say "t2-r$n: one port, split $(cat $P/svc_rtc_tx_split), nconnect ${NCONN:-4}"
cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.0 2>/dev/null
env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" RW=read BS=1M RSIZE=262144 \
	NCONN=${NCONN:-4} JOBS=${JOBS:-8} WARM=${WARM:-5} RUNTIME=${RUNTIME:-20} \
	RWTSV=$O/rw.tsv TAG=split1-$n ./twoport.sh $O/run a > $O/run.log 2>&1
rc=$?
cat /sys/kernel/debug/nfsd/rtc_stats > $O/rtc_stats.1 2>/dev/null
dmesg > $O/dmesg.1
say "twoport exit $rc: $(grep -E '^total|read:' $O/run.log | head -2 | tr '\n' ' ')"
diff <(grep -E 'WARNING:|BUG:|Oops|lockup|watchdog' $O/dmesg.0) <(grep -E 'WARNING:|BUG:|Oops|lockup|watchdog' $O/dmesg.1) | sed -n 's/^> /new: /p'
./settled.sh audit rest > $O/apply-after.txt 2>&1; tail -1 $O/apply-after.txt
exit $rc
