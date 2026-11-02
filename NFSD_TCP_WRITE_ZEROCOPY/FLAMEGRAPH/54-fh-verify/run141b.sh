#!/bin/bash
# run141b.sh: round 54, the part of run141 that its channel format lost: a key
# due for refresh, on the live path.  One mount, a dd loop, one key rewritten
# to expire in 16 s, so a refresh is due after 8 s.  An fsid goes down the
# channel as its raw bytes in hex.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r141; R=../FLAMEGRAPH/54-fh-verify
say() { echo "$(date +%T) $*"; }
C=${CLIP[a]} SV=${SRVP[a]} E=/hs/nvme4/zcbudget M=/mnt/rxr-t141
OPT=rw,sync,no_root_squash,insecure,no_subtree_check,fsid=7141
exportfs -o $OPT "$C:$E" || exit 1
$SSHC "mkdir -p $M; mount -t nfs -o vers=3,proto=tcp $SV:$E $M" || exit 1
bpftrace $R/paths.bt > $O/paths-b.txt 2>&1 &
BT=$!
until grep -q Attached $O/paths-b.txt; do sleep 0.5; done
$SSHC "for i in \$(seq 700); do dd if=/dev/zero of=$M/u bs=1M count=1 oflag=direct 2>/dev/null || echo FAIL; sleep 0.04; done | sort | uniq -c" > $O/dd-b.txt 2>&1 &
DD=$!
sleep 3
grep -v '^#' /proc/net/rpc/nfsd.fh/content | grep 'nvme4' > $O/fh-b.txt
read -r dom t fsid path < $O/fh-b.txt
hex=$(python3 -c "import sys,struct; print(struct.pack('<I', int(sys.argv[1],16)).hex())" $fsid)
exp=$(( $(date +%s) + 16 ))
say "key $dom $t $fsid as $hex, expiry +16 s"
echo "$dom $t \\x$hex $exp $path" > /proc/net/rpc/nfsd.fh/channel; say "channel rc=$?"
sleep 22
grep -v '^#' /proc/net/rpc/nfsd.fh/content | grep nvme4 | cut -c1-80
wait $DD; say "dd: $(cat $O/dd-b.txt)"
kill -INT $BT; wait $BT 2>/dev/null
$SSHC "rm -f $M/u; umount $M; rmdir $M"; exportfs -u "$C:$E"
say "end: warnings $(dmesg | grep -cE 'WARNING:|BUG:|Oops|rcu.*stall')"
