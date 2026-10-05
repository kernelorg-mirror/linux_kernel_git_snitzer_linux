#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# build-test-kernel.sh --base-config FILE [options]
#
# Build and install this branch's kernel on a test host, from the host's own
# known-good config plus the options the page-loan series and its harness
# need, under the hs.<N> name kernel-rpmtree gives it:
#
#   1. merge-config.sh: BASE + zerocopy.fragment -> resolved config, checked
#      (refuses to build if Kconfig dropped a required option);
#   2. kernel-rpmtree's build-kernel-on-host.sh --build-number N --config
#      <resolved> --skip-builddeps <tree>  (host-bootstrap.sh installed the
#      build dependencies);
#   3. unless --no-install: make modules_install INSTALL_MOD_STRIP=1,
#      make install, /boot/config-<release>, grubby --set-default;
#   4. a manifest (STATE/manifest) that preflight.sh checks after the boot:
#      release, page size, branch and commit, base config checksum, module
#      srcversions.
#
# Options:
#   --base-config FILE   the host's known-good .config (required)
#   --build-number N     default 451
#   --rpmtree DIR        kernel-rpmtree checkout (default /root/kernel/kernel-rpmtree)
#   --tree DIR           kernel tree (default: this checkout)
#   --jobs N             default nproc
#   --state DIR          default /var/lib/zc-transplant
#   --no-install         build only
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BASE=""; BN=451; RPMTREE=/root/kernel/kernel-rpmtree
TREE=$(cd "$HERE/../.." && pwd); JOBS=$(nproc); STATE=/var/lib/zc-transplant; INSTALL=1
while [ $# -gt 0 ]; do
	case "$1" in
	--base-config) BASE=$2; shift 2 ;;
	--build-number) BN=$2; shift 2 ;;
	--rpmtree) RPMTREE=$2; shift 2 ;;
	--tree) TREE=$2; shift 2 ;;
	--jobs) JOBS=$2; shift 2 ;;
	--state) STATE=$2; shift 2 ;;
	--no-install) INSTALL=0; shift ;;
	*) sed -n '3,30p' "$0"; exit 2 ;;
	esac
done
[ -n "$BASE" ] || { echo "--base-config is required" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
[ -x "$RPMTREE/build-kernel-on-host.sh" ] || { echo "no kernel-rpmtree at $RPMTREE" >&2; exit 2; }
mkdir -p "$STATE"
stamp=$(date +%Y%m%d-%H%M%S)
CFG=$STATE/config-$stamp
LOG=$STATE/build-$stamp.log

echo "== 1. config: $BASE + zerocopy.fragment"
"$HERE/merge-config.sh" "$BASE" "$CFG" "$TREE"
cfg_ps=4096
grep -q '^CONFIG_ARM64_16K_PAGES=y' "$CFG" && cfg_ps=16384
grep -q '^CONFIG_ARM64_64K_PAGES=y' "$CFG" && cfg_ps=65536
host_ps=$(getconf PAGESIZE)
[ "$cfg_ps" = "$host_ps" ] || echo "note: building a $cfg_ps-byte-page kernel on a $host_ps-byte-page host"

echo "== 2. build (log $LOG)"
"$RPMTREE/build-kernel-on-host.sh" --build-number "$BN" --config "$CFG" \
	--skip-builddeps --jobs "$JOBS" "$TREE" 2>&1 | tee "$LOG"
KREL=$(make -s -C "$TREE" kernelrelease)
echo "built $KREL"

if [ $INSTALL = 1 ]; then
	echo "== 3. install $KREL"
	make -C "$TREE" -s modules_install INSTALL_MOD_STRIP=1
	make -C "$TREE" -s install
	cp "$TREE/.config" "/boot/config-$KREL"
	grubby --set-default "/boot/vmlinuz-$KREL"
	echo "default boot entry: $(grubby --default-kernel)"
fi

echo "== 4. manifest"
srcv() { modinfo -F srcversion "$(find "$TREE/$1" -name "$2.ko" | head -1)" 2>/dev/null; }
{
	echo "release=$KREL"
	echo "page_size=$cfg_ps"
	echo "branch=$(git -C "$TREE" rev-parse --abbrev-ref HEAD)"
	echo "commit=$(git -C "$TREE" rev-parse HEAD)"
	echo "describe=$(git -C "$TREE" describe --always --dirty)"
	echo "base_config=$(readlink -f "$BASE") sha256=$(sha256sum "$BASE" | cut -d' ' -f1)"
	echo "config=$CFG"
	echo "built=$stamp installed=$INSTALL"
	for m in net/sunrpc:sunrpc fs/nfsd:nfsd fs/nfs:nfs net/sunrpc:svcsock_kunit; do
		echo "srcversion_${m#*:}=$(srcv "${m%%:*}" "${m#*:}")"
	done
} | tee "$STATE/manifest"
[ $INSTALL = 1 ] && echo "== reboot into $KREL, then run: $HERE/preflight.sh"
exit 0
