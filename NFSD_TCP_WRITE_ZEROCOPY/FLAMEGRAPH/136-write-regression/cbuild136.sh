#!/bin/bash
# cbuild136.sh R: FLAMEGRAPH round 136, build 469's NFS client stack
# against the running kernel (build 541) on the client.  R holds a tree's
# net/sunrpc, fs/nfs_common, fs/lockd, fs/nfs and the include/ files they
# use (sunrpc, lockd, nfs*, the trace headers), and COMMIT.  As round
# 132's cbuild132.sh, but every header in R/include is laid over the copy
# of the running kernel's kernel-devel, and nfsd is not built (the client
# does not load it).  Into R/root, a module root for cswap131.sh.
set -eu
k=$(uname -r); R=${1:?cbuild136.sh R}
rm -rf $R/devel $R/root
cp -a /usr/src/kernels/$k $R/devel
# Build 541's VFS passes inode operations a const struct mnt_idmap *;
# a tree from before that change is adapted for the build only (types,
# no behaviour), and one after it is left as it is.
grep -rl "struct mnt_idmap \*" $R/fs/nfs $R/include/linux/nfs*.h 2>/dev/null |
	xargs -r sed -i -E "s/struct mnt_idmap \*/const struct mnt_idmap */g; s/(const )+const struct mnt_idmap/const struct mnt_idmap/g"
cp -a $R/include/. $R/devel/include/
for d in fs/lockd; do sed -i '/^\.PHONY: xdrgen/,$d' $R/$d/Makefile 2>/dev/null || true; done
X=""
for d in net/sunrpc fs/nfs_common fs/lockd fs/nfs; do
	make -C $R/devel M=$R/$d KBUILD_EXTRA_SYMBOLS="$X" -j$(nproc) modules > $R/build-${d//\//-}.log 2>&1 ||
		{ echo "$d failed:"; grep -E 'error|Error' $R/build-${d//\//-}.log | head -15; exit 1; }
	X="$X $R/$d/Module.symvers"
done
M=$R/root/lib/modules/$k; mkdir -p $M
cp -as /lib/modules/$k/kernel $M/kernel
cp /lib/modules/$k/modules.builtin* /lib/modules/$k/modules.order $M/
mkdir -p $M/updates
find $R/net $R/fs -name '*.ko' ! -name '*_kunit.ko' | while read f; do
	n=$(basename $f .ko); cp $f $M/updates/
	find $M/kernel -name "$n.ko*" -delete
done
depmod -b $R/root $k
echo "($(cat $R/COMMIT)): $(ls $M/updates | wc -l) modules: $(ls $M/updates | sed 's/\.ko$//' | paste -sd' ')"
echo "warnings: $(cat $R/build-*.log | grep -c -i 'warning:') (compiler/pahole notes: $(cat $R/build-*.log | grep -c -E 'compiler differs|pahole version'))"
echo "sunrpc srcversion $(modinfo -F srcversion $M/updates/sunrpc.ko) (build 469's: AB674CEE87351D814F68CEE)"
