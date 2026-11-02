#!/bin/bash
# Round 54, cut short for a shutdown at Mike's word: l3-B to its end, then the
# profile on B, then l4-A if there is time.
S=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/d422d5e4-dcd9-4151-84e7-ab3e472a1d51/scratchpad
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r142
while kill -0 1151278 2>/dev/null; do sleep 0.5; done
kill -KILL 1007416
echo "$(date +%T) l3-B: $(grep -hE 'p50' $O/l3-B.out | head -1)"
./settled.sh apply > $O/apply-fl54.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply-fl54.txt | head -3; }
./flame.sh $O fl54 > $O/fl54.out 2>&1; echo "$(date +%T) flame rc=$?: $(./cost.sh $O/fl54 2>/dev/null | cut -c1-130)"
SUNRPC_KO=$S/sunE/sunrpc/sunrpc.ko NFSD_KO=none ./swap-sunrpc.sh > $O/swap.txt 2>&1; systemctl start nfs-server; sleep 2
echo "$(date +%T) nfsd $(cat /sys/module/nfsd/srcversion)"
./settled.sh apply > $O/apply-l4-A.txt 2>&1 || grep -E 'BAD|FAILED' $O/apply-l4-A.txt | head -3
./lat.sh $O l4-A > $O/l4-A.out 2>&1
echo "$(date +%T) l4-A: $(grep -hE 'p50' $O/l4-A.out | head -1)"
echo "$(date +%T) end: $(./settled.sh audit rest 2>&1 | tail -1); warnings $(dmesg | grep -cE 'WARNING:|BUG:|Oops')"
