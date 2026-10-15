#!/bin/bash
# build-sunrpc.sh [outdir] - build sunrpc.ko from this tree for the running
# kernel, as an external module against its -devel package: a change that is
# confined to net/sunrpc can then be tried without a kernel build.  It only
# builds: it installs nothing and loads nothing.
#
# The running kernel's own headers are used for everything but
# include/linux/sunrpc and sunrpc's own trace events, so what is built in
# (the page pool, TCP) is seen as that kernel has it.  The exported symbols' versions are compared with the
# installed module's: if they all match, nfsd and the other modules that
# depend on sunrpc load against the new one as they are.
#
# Putting it in place means stopping nfsd, unmounting every NFS mount,
# unloading every module that depends on sunrpc and loading them again,
# which drops every client's connections: that is Mike's to do.
. "$(dirname "$0")/common.sh"
TREE=$(cd "$RIG/../.." && pwd)
O=${1:-$L/sunrpc-$(uname -r)}
KB=/lib/modules/$(uname -r)/build
[ -e $KB/Module.symvers ] || { echo "the running kernel's -devel package must be installed"; exit 1; }
rm -rf $O; mkdir -p $O/inc/linux
cp -a $TREE/net/sunrpc $O/sunrpc
cp -a $TREE/include/linux/sunrpc $O/inc/linux/
mkdir -p $O/inc/trace/events
cp -a $TREE/include/trace/events/sunrpc.h $TREE/include/trace/events/sunrpc_base.h $O/inc/trace/events/
git -C $TREE log --oneline -1 -- net/sunrpc include/linux/sunrpc
# A module gets its BTF against vmlinux, which -devel does not carry, and
# without BTF the tracers' fentry:sunrpc: probes do not attach.  The running
# kernel's own BTF stands in for it while the module is built.
if [ ! -e $KB/vmlinux ] && [ -e /sys/kernel/btf/vmlinux ]; then
	ln -s /sys/kernel/btf/vmlinux $KB/vmlinux
	trap 'rm -f $KB/vmlinux' EXIT
fi
make -C $KB M=$O/sunrpc NOSTDINC_FLAGS="-nostdinc -I$O/inc" -j$(nproc) modules > $O/build.log 2>&1 ||
	{ echo "build failed:"; grep -i -m5 error $O/build.log; exit 1; }
echo "built: $O/sunrpc/sunrpc.ko"
readelf -S $O/sunrpc/sunrpc.ko | grep -q ' \.BTF ' && echo "BTF: in sunrpc.ko" || echo "BTF: ABSENT (the tracers will not attach)"
modinfo $O/sunrpc/sunrpc.ko | grep -E '^vermagic|^parm: +svc_tcp_rx_linger_us'
awk '$3 ~ /sunrpc$/ && $3 !~ /auth_gss|xprtrdma/ {print $2, $1}' $KB/Module.symvers | sort > $O/old.syms
awk '{print $2, $1}' $O/sunrpc/Module.symvers | sort > $O/new.syms
echo "exports of the installed module: $(wc -l < $O/old.syms); with another version in the new one: $(join $O/old.syms $O/new.syms | awk '$2 != $3' | wc -l); missing from it: $(join -v1 $O/old.syms $O/new.syms | wc -l)"
