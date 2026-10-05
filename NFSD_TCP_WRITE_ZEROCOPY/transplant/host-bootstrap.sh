#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# host-bootstrap.sh [--check]
#
# Prepare a fresh EL9 aarch64 host (the 16K test host, later tardis1) to
# build this branch's kernel and run the page-loan test plan.  Installs the
# packages the build and every harness script use, enables CRB if a package
# needs it, adds an nfs.conf drop-in (NFS versions 3, 4.0, 4.1, 4.2 on; the
# harness exports with absolute paths) unless one exists, and checks the
# filesystems the rigs need are mounted.
#
# --check: change nothing; report what is missing and exit 1 if anything is.
set -u
CHECK=0
[ "${1:-}" = --check ] && CHECK=1
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
missing=0

# Harness (run-*.sh, rigs, tracing) -- command : EL9 package
TOOLS="fio:fio clang:clang bpftrace:bpftrace nvme:nvme-cli dmsetup:device-mapper
mkfs.xfs:xfsprogs xfs_io:xfsprogs exportfs:nfs-utils rpc.nfsd:nfs-utils
mount.nfs:nfs-utils rpcbind:rpcbind ip:iproute ss:iproute nstat:iproute
unshare:util-linux nsenter:util-linux fincore:util-linux column:util-linux
cmp:diffutils gawk:gawk pgrep:procps-ng python3:python3 git:git
gcc:gcc make:make ethtool:ethtool rsync:rsync"
# Kernel build on this host (build-kernel-on-host.sh --skip-builddeps)
BUILD_PKGS="bison flex bc openssl openssl-devel elfutils-libelf-devel dwarves perl-interpreter
python3 xz zstd cpio kmod dracut grubby libaio"

need_pkgs=""
for t in $TOOLS; do
	c=${t%%:*}; p=${t#*:}
	command -v "$c" >/dev/null 2>&1 || { echo "missing command $c ($p)"; need_pkgs="$need_pkgs $p"; }
done
for p in $BUILD_PKGS; do
	rpm -q "$p" >/dev/null 2>&1 || { echo "missing package $p"; need_pkgs="$need_pkgs $p"; }
done
need_pkgs=$(echo "$need_pkgs" | tr ' ' '\n' | sort -u | tr '\n' ' ')

if [ -n "${need_pkgs// /}" ]; then
	if [ $CHECK = 1 ]; then
		missing=1
	else
		if dnf repolist --all 2>/dev/null | grep -qE '^crb\s.*disabled'; then
			echo "enabling CRB (dwarves, libaio-devel and others live there)"
			dnf config-manager --set-enabled crb
		fi
		# shellcheck disable=SC2086
		dnf -y install $need_pkgs || { echo "dnf install failed" >&2; missing=1; }
	fi
fi

# NFS server versions: the harness mounts v3 and v4.2; run-kunit.sh and the
# KUnit nfsd4 suite need v4.0-v4.2.  A drop-in, never an edit of nfs.conf.
DROPIN=/etc/nfs.conf.d/zc-harness.conf
if [ ! -e "$DROPIN" ]; then
	if [ $CHECK = 1 ]; then
		echo "missing $DROPIN (NFS versions 3, 4.0, 4.1, 4.2 on)"; missing=1
	else
		mkdir -p /etc/nfs.conf.d
		cat > "$DROPIN" <<'EOF'
# page-loan test harness (NFSD_TCP_WRITE_ZEROCOPY/transplant/host-bootstrap.sh)
[nfsd]
vers3=y
vers4.0=y
vers4.1=y
vers4.2=y
EOF
		echo "wrote $DROPIN"
	fi
fi
for f in /etc/exports /var/lib/nfs; do [ -e $f ] || { [ $CHECK = 1 ] && { echo "missing $f"; missing=1; } || { [ $f = /etc/exports ] && touch $f || mkdir -p $f; }; }; done
mkdir -p /etc/exports.d 2>/dev/null

# Filesystems the rigs and tracing use
for m in /sys/kernel/config:configfs /sys/kernel/debug:debugfs /sys/kernel/tracing:tracefs; do
	d=${m%%:*}; t=${m#*:}
	mountpoint -q "$d" && continue
	if [ $CHECK = 1 ]; then echo "not mounted: $d ($t)"; missing=1
	else mount -t "$t" "$t" "$d" && echo "mounted $d"; fi
done

# Optional: drgn (live-kernel inspection, as used for the credit wait list)
python3 -c 'import drgn' 2>/dev/null || echo "optional: drgn not installed (pip3 install drgn)"

echo "page size $(getconf PAGESIZE), $(nproc) CPUs, $(awk '/MemTotal/ {printf "%d GiB", $2/1048576}' /proc/meminfo) RAM," \
     "$(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ') free in /var/tmp"
if [ $missing = 0 ]; then echo "== host ready"; else echo "== host NOT ready (see above)"; fi
exit $missing
