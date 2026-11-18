#!/bin/bash
# cbuild132.sh R: round 131's cbuild131.sh with the tree in R (default
# /root/zc-ckern/T), so that round 132 can build two (FLAMEGRAPH round
# 132).  cbuild131.sh: run on the client in R, which holds this
# tree's net/sunrpc, fs/{nfs_common,lockd,nfs,nfsd}, include/linux/sunrpc,
# include/linux/skbuff_ref.h and include/trace/events/sunrpc*.h, and
# COMMIT (FLAMEGRAPH round 131).  As round 91's cbuild.sh: a copy of the
# running kernel's kernel-devel with those headers laid over it, every
# module of those directories built against it in turn (each against the
# Module.symvers before it), and a module root, T/root, in which they
# stand in for the rpm's (links to the rest of /lib/modules/$k), for
# cswap.sh's modprobe -d.  Nothing outside /root/zc-ckern/T.
set -eu
k=$(uname -r); R=${1:-/root/zc-ckern/T}
rm -rf $R/devel $R/root
cp -a /usr/src/kernels/$k $R/devel
cp -a $R/include/linux/sunrpc/. $R/devel/include/linux/sunrpc/
cp $R/include/linux/skbuff_ref.h $R/devel/include/linux/
cp $R/include/trace/events/sunrpc.h $R/devel/include/trace/events/
cp $R/include/trace/misc/sunrpc.h $R/devel/include/trace/misc/
for d in fs/lockd fs/nfsd; do sed -i '/^\.PHONY: xdrgen/,$d' $R/$d/Makefile; done
X=""
for d in net/sunrpc fs/nfs_common fs/lockd fs/nfs fs/nfsd; do
	make -C $R/devel M=$R/$d KBUILD_EXTRA_SYMBOLS="$X" -j$(nproc) modules > $R/build-${d//\//-}.log 2>&1 ||
		{ echo "$d failed:"; grep -E 'error|Error' $R/build-${d//\//-}.log | head; exit 1; }
	X="$X $R/$d/Module.symvers"
done
M=$R/root/lib/modules/$k; mkdir -p $M
cp -as /lib/modules/$k/kernel $M/kernel
cp /lib/modules/$k/modules.builtin* /lib/modules/$k/modules.order $M/
mkdir -p $M/updates
find $R/net $R/fs -name '*.ko' | while read f; do
	n=$(basename $f .ko); cp $f $M/updates/
	find $M/kernel -name "$n.ko*" -delete
done
depmod -b $R/root $k
echo "T ($(cat $R/COMMIT)): $(ls $M/updates | wc -l) modules: $(ls $M/updates | sed 's/\.ko$//' | paste -sd' ')"
echo "T warnings: $(cat $R/build-*.log | grep -c -i 'warning:') (compiler/pahole notes: $(cat $R/build-*.log | grep -c -E 'compiler differs|pahole version'))"
echo "sunrpc srcversion $(modinfo -F srcversion $M/updates/sunrpc.ko)"
rm -f $M/updates/*_kunit.ko; depmod -b $R/root $k
echo "without the KUnit modules: $(ls $M/updates | wc -l)"
