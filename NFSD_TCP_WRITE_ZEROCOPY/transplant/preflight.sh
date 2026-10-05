#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# preflight.sh [STATE]   -- after booting the test kernel, before the test plan
#
# Checks, and exits 1 on any FAIL:
#   - the running kernel is the one build-test-kernel.sh installed (STATE/manifest,
#     default /var/lib/zc-transplant; on tardis1's packaged RPM kernel there is
#     no manifest and this is skipped);
#   - its config has every zerocopy.fragment option (/boot/config-$(uname -r));
#   - every module the suites and rigs load is installed for it, and the
#     installed sunrpc/nfsd/nfs/svcsock_kunit are the build's (srcversion);
#   - the series' interfaces exist (sunrpc and page_pool parameters, the
#     region and nfsd debugfs files, the tracepoints);
#   - host-bootstrap.sh --check passes (commands, nfs.conf, mounts);
#   - memory and disk for the rigs; whether this boot is still fresh (no
#     receive region yet), which budget phases B and D need.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
STATE=${1:-/var/lib/zc-transplant}
KREL=$(uname -r)
PS=$(getconf PAGESIZE)
fails=0
ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; fails=$((fails + 1)); }
note() { echo "  note  $*"; }

echo "== kernel $KREL ($(uname -v)), page size $PS"

echo "== build manifest"
if [ -r "$STATE/manifest" ]; then
	mrel=$(sed -n 's/^release=//p' "$STATE/manifest")
	mps=$(sed -n 's/^page_size=//p' "$STATE/manifest")
	[ "$mrel" = "$KREL" ] && ok "running the installed build $mrel" || bad "running $KREL, manifest says $mrel (reboot into it?)"
	[ "$mps" = "$PS" ] && ok "page size $PS as built" || bad "page size $PS, built for $mps"
	for m in sunrpc nfsd nfs svcsock_kunit; do
		want=$(sed -n "s/^srcversion_$m=//p" "$STATE/manifest")
		have=$(modinfo -F srcversion "$m" 2>/dev/null)
		[ -z "$want" ] && continue
		[ "$want" = "$have" ] && ok "$m srcversion $have" || bad "$m installed srcversion ${have:-none}, build has $want"
	done
	grep -E '^(branch|commit|describe)=' "$STATE/manifest" | sed 's/^/  info  /'
else
	note "no $STATE/manifest (packaged kernel?): skipping build identity checks"
	pkg=$(rpm -qf "/lib/modules/$KREL" 2>/dev/null) && echo "  info  modules owned by $pkg"
fi

echo "== config"
CFG=/boot/config-$KREL
[ -r "$CFG" ] || [ ! -r "$STATE/manifest" ] || CFG=$(sed -n 's/^config=//p' "$STATE/manifest")
if [ -r "$CFG" ]; then
	"$HERE/check-config.sh" "$CFG" | sed 's/^/  /'
	[ "${PIPESTATUS[0]}" = 0 ] || fails=$((fails + 1))
else
	bad "no $CFG to check"
fi

echo "== modules"
f0=$fails
for m in kunit xdr_kunit svcsock_kunit nfsd_bvec_kunit nfsd4_bvec_kunit \
	 sunrpc nfsd nfs nfsv3 nfsv4 veth null_blk dm_delay brd zram \
	 nvmet nvme_loop nvmet_tcp nvme_tcp xfs; do
	if modinfo "$m" >/dev/null 2>&1; then :; else
		case $m in *kunit) bad "$m not installed (packaged kernel: install kernel*-modules-internal)" ;;
		*) bad "$m not installed" ;; esac
	fi
done
[ $fails = "$f0" ] && ok "all suite and rig modules installed"

echo "== series interfaces"
modprobe sunrpc 2>/dev/null; modprobe nfsd 2>/dev/null; modprobe nfs 2>/dev/null
for p in /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages \
	 /sys/module/sunrpc/parameters/svc_tcp_rx_budget_pages \
	 /sys/module/sunrpc/parameters/svc_tcp_rx_budget_default_pages \
	 /sys/module/sunrpc/parameters/svc_tcp_rx_budget_reserve \
	 /sys/module/sunrpc/parameters/svc_tcp_rx_credit_factor \
	 /sys/module/sunrpc/parameters/svc_tcp_rx_rcvbuf \
	 /sys/module/page_pool/parameters/rx_region_dev \
	 /sys/module/page_pool/parameters/rx_region_pages \
	 /sys/kernel/debug/nfsd/io_cache_write \
	 /sys/module/nfs/parameters/localio_enabled \
	 /proc/sys/net/sunrpc/region_budget_pages \
	 /sys/kernel/tracing/events/sunrpc/svcsock_tcp_rx_lifetime \
	 /sys/kernel/tracing/events/nfsd/nfsd_write_direct; do
	[ -e "$p" ] && ok "$p" || bad "missing $p"
done
if [ -e /sys/kernel/debug/page_pool_rx_region ]; then
	note "a receive region exists this boot ($(awk '$1=="pages"||$1=="dev"{printf "%s %s ", $1, $2}' /sys/kernel/debug/page_pool_rx_region)): not fresh, budget phases B and D need a reboot first"
else
	ok "fresh boot: no receive region yet (budget phases B and D can run)"
fi
[ "$(cat /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages 2>/dev/null)" = Y ] && ok "page loans on by default" || bad "svc_tcp_rx_loan_pages is not Y"

echo "== host"
"$HERE/host-bootstrap.sh" --check | sed 's/^/  /'
[ "${PIPESTATUS[0]}" = 0 ] || fails=$((fails + 1))
avail=$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)
[ "$avail" -ge 8 ] && ok "${avail} GiB memory available (rigs take up to 6 GiB at once)" || bad "only ${avail} GiB memory available; the RAM-backed rigs need 8"
shm=$(df -BG --output=avail /dev/shm | tail -1 | tr -dc 0-9)
[ "$shm" -ge 5 ] && ok "${shm} GiB free in /dev/shm" || note "${shm} GiB in /dev/shm: the LOCALIO A/B stage's 4 GiB image may not fit"
vt=$(df -BG --output=avail /var/tmp | tail -1 | tr -dc 0-9)
[ "$vt" -ge 5 ] && ok "${vt} GiB free in /var/tmp (results)" || bad "only ${vt} GiB free in /var/tmp"

if [ $fails = 0 ]; then echo "== PREFLIGHT PASS"; else echo "== PREFLIGHT FAIL ($fails)"; fi
exit $(( fails > 0 ))
