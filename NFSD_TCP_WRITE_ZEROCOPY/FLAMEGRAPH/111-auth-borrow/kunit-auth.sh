#!/bin/bash
# kunit-auth.sh <stackdir> - run the svcauth_unix_borrow suite on the host.
# The suite module is built against the running sunrpc (<stackdir>'s
# rpc/sunrpc/svcauth_unix_kunit.ko).  kunit's enable cannot be set at
# runtime and nfsd holds kunit, so nfsd is unloaded first; kunit comes
# back enabled with a filter that runs this suite and nothing else (the
# stack's nfsd links nfsd_rtc_kunit in, which must not run), and is
# unloaded again before nfsd loads, so nfsd pulls in a disabled one.
# Every step goes through step.sh; the stack is put back with
# swap-sunrpc.sh, then settled.sh apply.
set -u
X=$(cd "$(dirname "$0")/../../xeu-rig" && pwd); cd "$X"
S=${1:?stackdir}; KO=$S/rpc/sunrpc/svcauth_unix_kunit.ko
LD=/var/tmp/zc-run-logs/kunit-auth-541; mkdir -p $LD
UNITS="nfs-server nfs-mountd nfs-idmapd nfsdcld rpc-statd rpc-gssd gssproxy"
st() { ./step.sh $LD "$@"; }
[ "$(modinfo -F srcversion $S/stack/sunrpc.ko)" = "$(cat /sys/module/sunrpc/srcversion)" ] ||
	{ echo "the running sunrpc is not $S's"; exit 1; }
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
st kunit-on modprobe kunit enable=1 filter_glob=svcauth_unix_borrow
st insmod-suite insmod $KO
dmesg > $LD/dmesg.after-suite
cp /sys/kernel/debug/kunit/svcauth_unix_borrow/results $LD/results 2>/dev/null
st rmmod-suite rmmod svcauth_unix_kunit
st kunit-off modprobe -r kunit
restore
grep -E '^\s*(ok|not ok) ' $LD/results
grep -cE 'WARNING:|BUG:|Oops' $LD/dmesg.after-suite
