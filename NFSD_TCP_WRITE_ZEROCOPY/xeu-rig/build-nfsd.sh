#!/bin/bash
# build-nfsd.sh [outdir] - build nfsd.ko from this tree for the running
# kernel, as an external module against its -devel package, as
# build-sunrpc.sh builds sunrpc.ko: a change confined to fs/nfsd can then be
# tried without a kernel build.  It only builds: it installs nothing and
# loads nothing.
#
# The running kernel's own headers are used for everything but
# include/linux/sunrpc, which comes from this tree as for sunrpc.ko, so the
# two modules agree on svc_rqst and the transports.  Check first that no other
# header nfsd uses has changed since the running kernel was built (git diff
# --stat <its commit> -- include).  The versions nfsd imports are compared
# with what the running sunrpc exports.
#
# swap-sunrpc.sh puts it in place (NFSD_KO=), which drops every client's
# connections: that is Mike's to do.
. "$(dirname "$0")/common.sh"
TREE=$(cd "$RIG/../.." && pwd)
O=${1:-$L/nfsd-$(uname -r)}
KB=/lib/modules/$(uname -r)/build
[ -e $KB/Module.symvers ] || { echo "the running kernel's -devel package must be installed"; exit 1; }
rm -rf $O; mkdir -p $O/inc/linux
cp -a $TREE/fs/nfsd $O/nfsd
# The rules that regenerate the xdrgen files name the .x sources by a path
# outside the copy; the generated files are in it, so drop the rules.
sed -i '/^\.PHONY: xdrgen/,$d' $O/nfsd/Makefile
cp -a $TREE/include/linux/sunrpc $O/inc/linux/
git -C $TREE log --oneline -1 -- fs/nfsd include/linux/sunrpc
if [ ! -e $KB/vmlinux ] && [ -e /sys/kernel/btf/vmlinux ]; then
	ln -s /sys/kernel/btf/vmlinux $KB/vmlinux
	trap 'rm -f $KB/vmlinux' EXIT
fi
make -C $KB M=$O/nfsd NOSTDINC_FLAGS="-nostdinc -I$O/inc" -j$(nproc) modules > $O/build.log 2>&1 ||
	{ echo "build failed:"; grep -i -m5 error $O/build.log; exit 1; }
echo "built: $O/nfsd/nfsd.ko"
readelf -S $O/nfsd/nfsd.ko | grep -q ' \.BTF ' && echo "BTF: in nfsd.ko" || echo "BTF: ABSENT (the tracers will not attach)"
modinfo $O/nfsd/nfsd.ko | grep -E '^vermagic'
# What it imports, against what the kernel and the sunrpc.ko build-sunrpc.sh
# last built export: a mismatch would refuse to load.
SR=${SUNRPC_SYMVERS:-$L/sunrpc-$(uname -r)/sunrpc/Module.symvers}
modprobe --dump-modversions $O/nfsd/nfsd.ko | awk '{print $2, $1}' | sort > $O/imports.syms
cat $KB/Module.symvers $SR 2> /dev/null | awk '{print $2, $1}' | sort -k1,1 -u > $O/exports.syms
echo "imports: $(wc -l < $O/imports.syms); with another version than exported: $(join $O/imports.syms $O/exports.syms | awk '$2 != $3' | wc -l); not exported: $(join -v1 $O/imports.syms $O/exports.syms | wc -l)"
