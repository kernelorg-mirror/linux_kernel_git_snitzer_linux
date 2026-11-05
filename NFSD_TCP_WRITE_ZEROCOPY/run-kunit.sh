#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run-kunit.sh - run the page-loan series' four KUnit suites against the
# running kernel's installed modules (per TESTING.md "KUnit suites"), and
# fail unless every suite loads, reports, and passes.
#
# Needs: the suites installed for $(uname -r) (CONFIG_*_KUNIT_TEST=m; on an
# RPM kernel they are in kernel[-64k]-modules-internal), nfs-server stopped
# (nfsd's versions file is busy while it runs), and /var/tmp on a file system
# that does DONTCACHE such as XFS (nfsd_bvec_kunit's dontcache_dir, a skip
# otherwise, which fails the run).  Leaves the suites unloaded
# and the server as it found it: every writable sunrpc parameter is saved
# before the suites and written back after them (the svcsock suite once left
# svc_tcp_rx_rcvbuf at 0), and nfs-server is started again: where
# hammerspace-nfsd-tuning is installed by restarting it, which starts
# nfs-server with its threads, settings and CPUs, and elsewhere (tardis2) by
# starting nfs-server itself.  On a host with the xeu card xeu-rig/settled.sh
# then puts the settled setting back and audits it, and the run fails if the
# host is not at it.
#
# Expected: no failures.  Skips: on 4 KiB pages exactly one,
# svcsock_rx_arena_bound_test (table and arena are the same size); on 16 and
# 64 KiB pages none -- any skip there is reported as unexpected.
set -u
OUT=${1:-/var/tmp/kunit-$(date +%Y%m%d-%H%M%S).log}
PS=$(getconf PAGESIZE)
SUITES="sunrpc-xdr-bvec sunrpc-svcsock-rx nfsd-receive-bvec nfsd4-receive-bvec"
fail=0

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
if systemctl is-active -q nfs-server 2>/dev/null; then
	echo "nfs-server is active: stop it first (systemctl stop nfs-server)" >&2
	exit 2
fi
{
echo "== kernel $(uname -r), page size $PS"
dmesg -C

# kunit must be loaded with enable=1 (param has 0 sysfs perms but is real)
modprobe -r xdr_kunit svcsock_kunit nfsd_bvec_kunit nfsd4_bvec_kunit 2>/dev/null
# The suites write the live module's settings: keep them to write back.
SAVED=$(for f in /sys/module/sunrpc/parameters/*; do [ -w $f ] && echo "$f $(cat $f)"; done | awk 'NF == 2')
modprobe -r kunit 2>/dev/null
modprobe kunit enable=1 || { echo "FAIL: modprobe kunit"; fail=1; }

for m in xdr_kunit svcsock_kunit nfsd_bvec_kunit; do
	modprobe $m || { echo "FAIL: modprobe $m"; fail=1; }
	sleep 1
done

# nfsd4_bvec_kunit needs a live nn->nfsd_serv (v4-only hand-start, no rpcbind)
# and NFSv4.0, which its q35 tests' COMPOUNDs use.  "+4.0" comes last: with
# the Hammerspace extensions (canary 1bb07fd5eb81) "+4.2" clears 4.0.
modprobe nfsd
mountpoint -q /proc/fs/nfsd || mount -t nfsd nfsd /proc/fs/nfsd
echo 0 > /proc/fs/nfsd/threads
echo "-2 -3 +4 +4.1 +4.2 +4.0" > /proc/fs/nfsd/versions
grep -q 2049 /proc/fs/nfsd/portlist 2>/dev/null || echo "tcp 2049" > /proc/fs/nfsd/portlist
echo 4 > /proc/fs/nfsd/threads
sleep 1
modprobe nfsd4_bvec_kunit || { echo "FAIL: modprobe nfsd4_bvec_kunit"; fail=1; }
sleep 2
echo 0 > /proc/fs/nfsd/threads

echo "=================== KTAP ==================="
dmesg | grep -E 'KTAP|ok [0-9]|not ok|# (Totals|sunrpc|nfsd)'
echo "=================== summary ==================="
for s in $SUITES; do
	line=$(dmesg | grep -E "# $s: pass:" | tail -1)
	if [ -z "$line" ]; then
		echo "FAIL: suite $s did not report"; fail=1; continue
	fi
	echo "$line"
	echo "$line" | grep -q ' fail:0 ' || { echo "FAIL: suite $s has failures"; fail=1; }
done
notok=$(dmesg | grep -E '^\[.*\]\s+not ok [0-9]+ ' | grep -v '# SKIP')
[ -n "$notok" ] && { echo "$notok" | sed 's/^/FAIL: /'; fail=1; }
skips=$(dmesg | grep -E 'ok [0-9]+ [a-z0-9_]+ # SKIP' | grep -oE 'ok [0-9]+ [a-z0-9_]+' | awk '{print $3}')
for t in $skips; do
	if [ "$PS" = 4096 ] && [ "$t" = svcsock_rx_arena_bound_test ]; then
		echo "skip (expected on 4 KiB): $t"
	else
		echo "UNEXPECTED SKIP on $PS-byte pages: $t"; fail=1
	fi
done
for m in nfsd4_bvec_kunit nfsd_bvec_kunit svcsock_kunit xdr_kunit; do modprobe -r $m 2>/dev/null; done
echo "=================== the server put back ==================="
while read -r f v; do
	[ "$(cat $f)" = "$v" ] && continue
	echo "sunrpc $(basename $f): the suites left $(cat $f), back to $v"
	echo "$v" > $f || { echo "FAIL: $(basename $f) not put back"; fail=1; }
done <<< "$SAVED"
if systemctl cat hammerspace-nfsd-tuning.service > /dev/null 2>&1; then
	systemctl restart hammerspace-nfsd-tuning || { echo "FAIL: restart hammerspace-nfsd-tuning"; fail=1; }
else
	systemctl start nfs-server || { echo "FAIL: start nfs-server"; fail=1; }
fi
echo "nfs-server $(systemctl is-active nfs-server), nfsd threads $(cat /proc/fs/nfsd/threads)"
S=$(dirname "$(readlink -f "$0")")/xeu-rig/settled.sh
if [ -x "$S" ] && [ -e /sys/class/net/enp161s0f0 ]; then
	"$S" apply > ${OUT%.log}.settled.txt 2>&1 || { grep -E 'BAD|FAILED|not have|nfsd has' ${OUT%.log}.settled.txt; echo "FAIL: the host is not at the settled setting"; fail=1; }
	tail -1 ${OUT%.log}.settled.txt
fi
if [ $fail = 0 ]; then echo "== KUNIT PASS"; else echo "== KUNIT FAIL"; fi
exit $fail
} 2>&1 | tee "$OUT"
exit "${PIPESTATUS[0]}"
