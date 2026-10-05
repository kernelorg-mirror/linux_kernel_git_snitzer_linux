#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run-kunit.sh - run the page-loan series' four KUnit suites against the
# running kernel's installed modules (per TESTING.md "KUnit suites"), and
# fail unless every suite loads, reports, and passes.
#
# Needs: the suites installed for $(uname -r) (CONFIG_*_KUNIT_TEST=m; on an
# RPM kernel they are in kernel[-64k]-modules-internal), nfs-server stopped
# (nfsd's versions file is busy while it runs).  Leaves nfsd stopped, its
# versions as the suites need them, and the suites unloaded.
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
modprobe -r kunit 2>/dev/null
modprobe kunit enable=1 || { echo "FAIL: modprobe kunit"; fail=1; }

for m in xdr_kunit svcsock_kunit nfsd_bvec_kunit; do
	modprobe $m || { echo "FAIL: modprobe $m"; fail=1; }
	sleep 1
done

# nfsd4_bvec_kunit needs a live nn->nfsd_serv (v4-only hand-start, no rpcbind)
modprobe nfsd
mountpoint -q /proc/fs/nfsd || mount -t nfsd nfsd /proc/fs/nfsd
echo 0 > /proc/fs/nfsd/threads
echo "-2 -3 +4 +4.1 +4.2" > /proc/fs/nfsd/versions
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
if [ $fail = 0 ]; then echo "== KUNIT PASS"; else echo "== KUNIT FAIL"; fi
exit $fail
} 2>&1 | tee "$OUT"
exit "${PIPESTATUS[0]}"
