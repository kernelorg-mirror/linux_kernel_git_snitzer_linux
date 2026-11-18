#!/bin/bash
# kunit-export.sh <stackdir> - run the sunrpc-nfsd-export-borrow suite on the
# host.  It is linked into nfsd, so it runs as a test nfsd loads:
# <stackdir>/nfsd-ebt/nfsd.ko, the stack's nfsd built with
# CONFIG_NFSD_EXPORT_BORROW_KUNIT_TEST=y and CONFIG_NFSD_RTC_KUNIT_TEST
# off.  As kunit-auth.sh: nfs units stopped, nfsd unloaded, kunit back
# with enable=1 and a filter that runs this suite only, the test nfsd
# loaded and unloaded with no service started, kunit unloaded before
# the stack's own nfsd loads, the stack put back with swap-sunrpc.sh,
# settled.sh apply.  Every step through step.sh.
set -u
X=$(cd "$(dirname "$0")/../../xeu-rig" && pwd); cd "$X"
S=${1:?stackdir}; KO=$S/nfsd-ebt/nfsd.ko
LD=/var/tmp/zc-run-logs/kunit-export-541; mkdir -p $LD
UNITS="nfs-server nfs-mountd nfs-idmapd nfsdcld rpc-statd rpc-gssd gssproxy"
st() { ./step.sh $LD "$@"; }
[ "$(modinfo -F srcversion $S/stack/sunrpc.ko)" = "$(cat /sys/module/sunrpc/srcversion)" ] ||
	{ echo "the running sunrpc is not $S's"; exit 1; }
nm $KO | grep -q nfsd_rtc_suite && { echo "$KO links the rtc suite"; exit 1; }
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] ||
	{ echo "a client is connected"; exit 1; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { echo "kdump not armed"; exit 1; }
restore() {
	st restore env STACK=$S/stack SUNRPC_KO=none ./swap-sunrpc.sh
	st settled ./settled.sh apply
}
st stop-units systemctl stop $UNITS
st umount-nfsd umount /proc/fs/nfsd
st rmmod-nfsd rmmod nfsd || { echo "nfsd will not unload"; restore; exit 1; }
st rmmod-kunit modprobe -r kunit || { restore; exit 1; }
st kunit-on modprobe kunit enable=1 filter_glob=sunrpc-nfsd-export-borrow
st insmod-suite insmod $KO
dmesg > $LD/dmesg.after-suite
cp /sys/kernel/debug/kunit/sunrpc-nfsd-export-borrow/results $LD/results 2>/dev/null
st rmmod-suite rmmod nfsd
st kunit-off modprobe -r kunit
restore
grep -E '^\s*(ok|not ok) ' $LD/results
n=$(grep -n 'Subtest: sunrpc-nfsd-export-borrow' $LD/dmesg.after-suite | tail -1 | cut -d: -f1)
[ -n "$n" ] || { echo "the suite did not run"; exit 1; }
echo "warnings in the suite's run: $(tail -n +$n $LD/dmesg.after-suite | grep -cE 'WARNING:|BUG:|Oops')"
