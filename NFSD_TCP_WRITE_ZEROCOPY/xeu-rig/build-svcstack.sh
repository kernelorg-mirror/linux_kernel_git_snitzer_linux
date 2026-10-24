#!/bin/bash
# build-svcstack.sh [outdir] - build sunrpc.ko and every module of the NFS
# server's stack that imports from it (auth_rpcgss, rpcsec_gss_krb5,
# rpcrdma, lockd, nfs_acl, nfs_localio, nfsd) from this tree for the running
# kernel, as build-sunrpc.sh and build-nfsd.sh do.  It is for a change to
# include/linux/sunrpc that moves what other modules see, such as a field
# added to struct svc_pool: the kernel versions its exports with
# gendwarfksyms, from the types' layout, so such a change gives most of
# sunrpc's exports new versions and the kernel's own lockd, auth_rpcgss and
# the rest no longer load against it.  The stack is built against the new
# sunrpc's Module.symvers, nfsd also against lockd's and nfs_common's.
#
# It only builds: it installs nothing and loads nothing.  The set is left in
# OUTDIR/stack; swap-sunrpc.sh puts it in place with STACK=OUTDIR/stack and
# SUNRPC_KO=none, and takes it out again with STACK=none.  Nothing outside
# fs/lockd, fs/nfs_common and fs/nfsd may have changed in include/ since the
# running kernel was built but include/linux/sunrpc (git diff --stat <its
# commit> -- include).
. "$(dirname "$0")/common.sh"
TREE=$(cd "$RIG/../.." && pwd)
O=${1:-$L/svcstack-$(uname -r)}
KB=/lib/modules/$(uname -r)/build
[ -e $KB/Module.symvers ] || { echo "the running kernel's -devel package must be installed"; exit 1; }
rm -rf $O; mkdir -p $O/inc/linux $O/stack
"$RIG/build-sunrpc.sh" $O/rpc || exit 1
SR=$O/rpc/sunrpc/Module.symvers
cp -a $TREE/include/linux/sunrpc $O/inc/linux/
for d in lockd nfs_common nfsd; do cp -a $TREE/fs/$d $O/$d; done
# As in build-nfsd.sh: the xdrgen rules name sources outside the copy, and
# the generated files they would remake are in it.
for d in lockd nfsd; do sed -i '/^\.PHONY: xdrgen/,$d' $O/$d/Makefile; done
if [ ! -e $KB/vmlinux ] && [ -e /sys/kernel/btf/vmlinux ]; then
	ln -s /sys/kernel/btf/vmlinux $KB/vmlinux
	trap 'rm -f $KB/vmlinux' EXIT
fi
build() {	# build DIR SYMVERS...
	local d=$1; shift
	make -C $KB M=$O/$d KBUILD_EXTRA_SYMBOLS="$*" NOSTDINC_FLAGS="-nostdinc -I$O/inc" -j$(nproc) modules > $O/$d.log 2>&1 ||
		{ echo "$d: build failed:"; grep -i -m5 error $O/$d.log; exit 1; }
}
build lockd $SR
build nfs_common $SR
build nfsd $SR $O/lockd/Module.symvers $O/nfs_common/Module.symvers
for k in rpc/sunrpc/sunrpc.ko rpc/sunrpc/auth_gss/auth_rpcgss.ko rpc/sunrpc/auth_gss/rpcsec_gss_krb5.ko \
	 rpc/sunrpc/xprtrdma/rpcrdma.ko lockd/lockd.ko nfs_common/nfs_acl.ko nfs_common/nfs_localio.ko nfsd/nfsd.ko; do
	[ -e $O/$k ] || { echo "no $k"; exit 1; }
	cp $O/$k $O/stack/
done
# What each imports, against what the kernel and the stack export: any
# mismatch would refuse to load.
cat $KB/Module.symvers $SR $O/lockd/Module.symvers $O/nfs_common/Module.symvers |
	awk '{print $2, $1}' | sort -k1,1 -u > $O/exports.syms
# The stack's own exports win over the kernel's for the same symbol.
cat $SR $O/lockd/Module.symvers $O/nfs_common/Module.symvers | awk '{print $2, $1}' | sort -k1,1 -u > $O/stack.syms
join -v1 $O/exports.syms $O/stack.syms | cat - $O/stack.syms | sort -k1,1 > $O/want.syms
bad=0
for k in $O/stack/*.ko; do
	modprobe --dump-modversions $k | awk '{print $2, $1}' | sort > $O/imports.syms
	n=$(join $O/imports.syms $O/want.syms | awk '$2 != $3' | wc -l)
	m=$(join -v1 $O/imports.syms $O/want.syms | wc -l)
	echo "$(basename $k): imports $(wc -l < $O/imports.syms), another version $n, not exported $m, srcversion $(modinfo -F srcversion $k)"
	[ $n = 0 ] && [ $m = 0 ] || bad=1
done
[ $bad = 0 ] || { echo "the stack would not load as built"; exit 1; }
echo "built: $O/stack"
