#!/bin/bash
# teardown-split.sh <n>: round 104, nfsd stopped and started with split
# connections up and reads in flight (Mike, 16 November: "focus on module
# teardown with split enabled").  Port a, /hs/nvme0 only, twelve
# connections, fio reading 1 MiB at rsize 256 KiB in the background on the
# client; then nfsd is stopped in the middle of it, started again, the
# client cleared and split put back to N.  Every step that could take the
# host down goes through step.sh, one command to a step, so the client's
# netconsole log names the step if it does.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
n=${1:?run number}
P=/sys/module/sunrpc/parameters
FSX=/hs/nvme0 SRVA=192.168.1.$SRVHOST CLIA=192.168.1.106 MNT=/mnt/rxr-a1
grep -qw 'irqchip.gicv3_pseudo_nmi=1' /proc/cmdline || { say "no pseudo-NMI on this boot"; exit 1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/srcversion)" = 8843BD2AFB934E2FEFD942B ] || { say "not the split sunrpc"; exit 1; }
O=$L/t2-r$n; mkdir -p $O || exit 1
S() { ./step.sh $O "$@"; }
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "NFS connections are up"; exit 1; }
./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled"; exit 1; }
ping -c1 -W2 $CLIA > /dev/null || { say "port a does not answer"; exit 1; }
dmesg > $O/dmesg.0
trap 'echo N > $P/svc_rtc_tx_split' EXIT

S split-on sh -c "echo Y > $P/svc_rtc_tx_split" || exit 1
mkdir -p $FSX/zcbudget; chmod 777 $FSX/zcbudget
S export exportfs -o rw,sync,no_root_squash,insecure,no_subtree_check,fsid=771 "$CLIA:$FSX/zcbudget" || exit 1
S mount $SSHC "mkdir -p $MNT && mount -t nfs -o vers=3,nolock,proto=tcp,sec=sys,nconnect=12,rsize=262144 $SRVA:$FSX/zcbudget $MNT" || exit 1
# Lay the files out first (a write), then read them for longer than the
# rest of this script takes.
S layout $SSHC "fio --name=lay --directory=$MNT --rw=write --bs=1M --size=4G --numjobs=8 --direct=1 --ioengine=libaio --iodepth=8 --group_reporting > /var/tmp/td-$n-layout.txt 2>&1; tail -3 /var/tmp/td-$n-layout.txt" || exit 1
S fio-start $SSHC "nohup fio --name=lay --directory=$MNT --rw=read --bs=1M --size=4G --numjobs=8 --direct=1 --ioengine=libaio --iodepth=8 --time_based --runtime=600 --group_reporting > /var/tmp/td-$n-read.txt 2>&1 < /dev/null & echo started"
sleep 15
S conns sh -c "ss -Htn state established '( sport = :2049 )' | wc -l; grep -E '^(diverted|sends) ' /sys/kernel/debug/nfsd/rtc_stats"
sleep 5
S rate sh -c "grep -E '^(diverted|sends) ' /sys/kernel/debug/nfsd/rtc_stats"

# The teardown: nfsd stopped with twelve split connections busy.
S nfsd-stop systemctl stop nfs-server; rc=$?
S after-stop sh -c "cat /proc/fs/nfsd/threads; ss -Htn state established '( sport = :2049 )' | wc -l; dmesg | tail -5"
S nfsd-start systemctl start nfs-server
S after-start sh -c "cat /proc/fs/nfsd/threads; dmesg | tail -3"

S fio-kill $SSHC "pkill -9 -x fio; sleep 2; pgrep -x fio | wc -l"
S umount $SSHC "umount -f $MNT; umount -l $MNT 2>/dev/null; grep -c rxr-a1 /proc/mounts"
S split-off sh -c "echo N > $P/svc_rtc_tx_split"
S unexport exportfs -u "$CLIA:$FSX/zcbudget"
dmesg > $O/dmesg.1
diff <(grep -E 'WARNING:|BUG:|Oops|lockup|watchdog|svc_rtc' $O/dmesg.0) <(grep -E 'WARNING:|BUG:|Oops|lockup|watchdog|svc_rtc' $O/dmesg.1) | sed -n 's/^> /new: /p'
./settled.sh apply > $O/apply-after.txt 2>&1; tail -1 $O/apply-after.txt
say "nfsd stop exit $rc"
exit $rc
