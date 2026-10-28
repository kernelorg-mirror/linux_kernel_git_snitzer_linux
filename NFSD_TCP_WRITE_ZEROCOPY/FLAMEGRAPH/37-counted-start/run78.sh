#!/bin/bash
# run78.sh: round 37, round 01's starting point counted.  Run on build 494
# (booted once with bootctl set-oneshot, no rodata= on its command line, so
# rodata=full), after the boot's bring-up (README.md, "The reboot").  Round
# 01's state: sunrpc 82ACAC76B5F85A2645B58A4 (the tip of 22 October with
# "SUNRPC: Wake an idle service thread only once", t2-tipkernel/fix-sunrpc.ko,
# which a rebuild from 2fc9cd2e839b reproduces), build 494's own nfsd
# (EF2C425552D548DD099D0B3) and xeu (E0A4EBC4A7138D7BA4A90DF, no
# irq_msix_mask, no rx_prefetch), podman's netavark table with its conntrack
# hooks and no zc_notrack, skb_defer_max 128, nfsd's own reply cache size.
# Four untraced loan64 runs as every later round's are run (one port, the
# four DPRP5s, WARM=10 RUNTIME=30, 64 WRITEs of budget); twoport.sh counts
# every CPU's cycles and instructions over its window, and cost.sh gives
# them a WRITE beside the tick-sampled us.  The audit holds each run to
# round 01's state through settled.sh's EXP_ settings.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r78
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
K494=7.3.0-7.hs.494.loanpage_pool.el10.aarch64+16k
XB=E0A4EBC4A7138D7BA4A90DF N=EF2C425552D548DD099D0B3 K=82ACAC76B5F85A2645B58A4
KO=$L/t2-tipkernel/fix-sunrpc.ko
NAV=$L/t2-tipkernel/netavark-table.nft
export EXP_RODATA=default EXP_DRC_MAX_ENTRIES=0 EXP_XEU_irq_msix_mask=absent EXP_XEU_rx_prefetch=absent
# skb_defer_max 128 and no zc_notrack come with twoport.sh's SKB_DEFER_MAX= and
# NOTRACK=, which it passes to its own audits; apply puts the settled ones back.
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
tables() { echo "tables: $(nft list tables | awk '{print $3}' | paste -sd ' '); conntrack hooks: $(nft list hooks 2> /dev/null | grep -c conntrack)"; }
start_state() {	# round 01's netfilter: the netavark table, no zc_notrack
	nft delete table inet zc_notrack 2> /dev/null
	nft list table inet netavark > /dev/null 2>&1 || nft -f $NAV || return 1
	[ "$(nft list hooks 2> /dev/null | grep -c conntrack)" != 0 ]
}

[ "$(uname -r)" = $K494 ] || die "not build 494: $(uname -r)"
grep -q 'rodata=' /proc/cmdline && die "rodata= on the command line: $(cat /proc/cmdline)"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XB"
[ "$(modinfo -F srcversion $KO)" = $K ] || die "$KO is not $K"
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
if [ "$(cat /sys/module/sunrpc/srcversion)" != $K ] || [ "$(cat /sys/module/nfsd/srcversion)" != $N ]; then
	# updates/ holds round 17's sunrpc and round 13's nfsd from 25 October;
	# both are kept as t2-tipkernel/r17-sunrpc.ko and r13b-nfsd.ko
	SUNRPC_KO=$KO NFSD_KO=none ./swap-sunrpc.sh > $O/swap.txt 2>&1 || die "swap: $(tail -3 $O/swap.txt)"
fi
say "kernel $(uname -r), rodata $(grep -oE 'rodata=[^ ]*' /proc/cmdline || echo default), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
W0=$(dmesg | grep -c 'WARNING:')
for i in 1 2 3 4; do
	tag=r78-$i-loan64-start
	client_free || die "client not free"
	./settled.sh apply > $O/apply-$tag.txt 2>&1 || die "$tag not settled: $(grep -E 'BAD|FAILED' $O/apply-$tag.txt | head -3)"
	start_state || die "$tag: the netavark table not in place"
	say "$tag | $(tables)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 SKB_DEFER_MAX=128 NOTRACK=absent ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$tag: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
done
nft delete table inet netavark 2> /dev/null
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tables); $(tail -1 $O/apply-end.txt); DONE"
