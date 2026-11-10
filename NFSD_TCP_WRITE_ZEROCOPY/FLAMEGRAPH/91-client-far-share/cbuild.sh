#!/bin/bash
# cbuild.sh: run on the client (106) in /root/zc-ckern, holding A/ and F/
# (net/sunrpc, fs/{nfs,nfsd,lockd,nfs_common} and the sunrpc headers of
# one branch each).  For each: a copy of the running kernel's kernel-devel
# with that branch's sunrpc headers laid over it, every module of those
# directories built against it, and a module root, <v>/root, in which
# those modules stand in for the rpm's (a farm of links to the rest of
# /lib/modules/$k), for modprobe -d.  Nothing outside /root/zc-ckern.
set -eu
k=$(uname -r); B=/root/zc-ckern
for v in ${ARMS:-A F}; do
	R=$B/$v; rm -rf $R/devel $R/root
	cp -a /usr/src/kernels/$k $R/devel
	cp -a $R/include/linux/sunrpc/. $R/devel/include/linux/sunrpc/
	cp $R/include/trace/events/sunrpc.h $R/devel/include/trace/events/
	X=""
	for d in net/sunrpc fs/nfs_common fs/lockd fs/nfs fs/nfsd; do
		make -C $R/devel M=$R/$d KBUILD_EXTRA_SYMBOLS="$X" -j48 modules > $R/build-${d//\//-}.log 2>&1 ||
			{ echo "$v $d failed:"; grep -E 'error|Error' $R/build-${d//\//-}.log | head; exit 1; }
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
	echo "$v ($(cat $R/COMMIT)): $(ls $M/updates | wc -l) modules: $(ls $M/updates | sed 's/\.ko$//' | paste -sd' ')"
	echo "$v warnings: $(cat $R/build-*.log | grep -c -i 'warning:' ) (of which compiler/pahole version notes: $(cat $R/build-*.log | grep -c -E 'compiler differs|pahole version'))"
done
