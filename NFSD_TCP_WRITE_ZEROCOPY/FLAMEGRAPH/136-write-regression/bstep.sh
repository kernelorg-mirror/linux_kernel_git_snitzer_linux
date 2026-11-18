#!/bin/bash
# bstep.sh <commit> <dir> <n> [<n> ...]: FLAMEGRAPH round 136, one step
# of the client's bisect.  The client's NFS stack from <commit>'s tree
# (git archive, no checkout: net/sunrpc, fs/nfs_common, fs/lockd, fs/nfs
# and the sunrpc, lockd and nfs headers), built on the client with
# cbuild136.sh into /root/zc-ckern/<dir> against build 541's kernel,
# loaded with cswap131.sh (client side), then prof-write.sh <n> for
# each run, held to the rpciod the stack loads with (EXP_CLI_WQ); the
# client back on build 541's stack after.
cd /home/serviceadmin/snitm/kernel/linux || exit 1
. NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/common.sh > /dev/null 2>&1
c=${1:?commit}; d=${2:?dir}; shift 2
say() { echo "$(date +%T) $*"; }
inc=$(git ls-tree --name-only -r $c include/linux/ include/trace/ | grep -E '^include/linux/(sunrpc|lockd)/|^include/linux/nfs[^/]*\.h$|^include/trace/(events|misc)/(sunrpc|nfs)[^/]*\.h$|^include/linux/skbuff_ref\.h$')
git archive $c net/sunrpc fs/nfs_common fs/lockd fs/nfs $inc |
	$SSHC "rm -rf /root/zc-ckern/$d && mkdir -p /root/zc-ckern/$d && tar -x -C /root/zc-ckern/$d && echo $(git rev-parse --short=13 $c) > /root/zc-ckern/$d/COMMIT"
$SSHC 'cat > /root/zc-ckern/cbuild136.sh && chmod +x /root/zc-ckern/cbuild136.sh' < NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/136-write-regression/cbuild136.sh
$SSHC "/root/zc-ckern/cbuild136.sh /root/zc-ckern/$d" 2>&1 | tail -4
$SSHC "test -s /root/zc-ckern/$d/root/lib/modules/\$(uname -r)/updates/nfs.ko" || { say "$c: build failed"; exit 1; }
say "swap: $($SSHC "NFSM='sunrpc nfsv3 nfsv4' /root/zc-csunrpc/cswap131.sh /root/zc-ckern/$d/root/ 2>&1 | tail -1")"
w=$($SSHC 'cat /sys/devices/virtual/workqueue/rpciod/per_cpu')
cd NFSD_TCP_WRITE_ZEROCOPY/xeu-rig
for n in "$@"; do
	[ -e $L/t2-r$n ] && { say "t2-r$n exists"; break; }
	EXP_CLI_WQ="rpciod/per_cpu=$w" ../FLAMEGRAPH/136-write-regression/prof-write.sh $n > /dev/null 2>&1
	O=$L/t2-r$n
	say "t2-r$n $(git rev-parse --short=13 $c) ($d, rpciod per_cpu $w): $(grep -oE 'write check: [A-Z]+ \| total [0-9.]+ GB/s' $O/write-check.out) | p99 $(awk '/^  write:/ {printf "%s%s", n++ ? "/" : "", $(NF-5)}' $O/run.log) ms | $(grep -oE 'new kernel warnings [0-9]+' $O/write-check.out) $(grep -oE 'run failed.*' $O/write-check.out)"
	say "  t9 queues: $(awk '/^sending_q_len/ {split($0,a,"="); print a[2]}' $O/xprt-9.txt 2>/dev/null | sort -n | awk '{a[NR]=$1; s+=$1} END {printf "sum %d, min %d median %d max %d", s, a[2], a[int(NR/2)+1], a[NR]}')"
done
say "swap stock: $($SSHC "NFSM='sunrpc nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma' /root/zc-csunrpc/cswap131.sh stock 2>&1 | tail -1")"
./settled.sh audit rest 2>&1 | tail -1
say "bstep $d done"
