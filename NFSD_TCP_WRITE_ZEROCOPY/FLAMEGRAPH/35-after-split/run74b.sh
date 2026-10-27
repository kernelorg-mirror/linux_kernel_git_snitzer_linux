#!/bin/bash
# run74b.sh: the rest of run74.sh's A/B (3-B, 4-A, 4-B), which stopped
# before 3-B when the client had another server's share mounted; the same
# xeu, the same runs, into the same directory.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r74
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
PORT=enp161s0f0
KO=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/9dd234f9-8d71-4758-ae14-f72417b58890/scratchpad/xeu35lean/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XB=F713CD92558EF73C0D04443 XK=DC23D10E104D370F14FB06E N=3F0C341E86E266D5FDF2ADF K=A06FE478658242A10E100FF
XL=/sys/module/xeu/parameters/rx_refill_lean
say() { echo "$(date +%T) $*"; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh enp161s0f0 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
pp() { ethtool -S $PORT | awk -F: '/_rx_pp_(alloc_slow|alloc_fast|recycle_ring_full|recycle_cached)/ {gsub(/ring_[0-9]+_rx_pp_| /, "", $1); s[$1] += $2} END {for (k in s) printf "%s=%s ", k, s[k]}'; }
snap() { echo "$(date +%s.%N) writes=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) $(pp)"; cat $RG; }
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	grep -qw 'rodata=noalias' /proc/cmdline || die "$1: not booted with rodata=noalias"
	[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XB"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	return 0
}
run() {	# run TAG
	local tag=$1 rc
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
	snap > $O/$tag-snap0.txt
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	sleep 5; snap > $O/$tag-snap1.txt
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1)))"
}

W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
xeu_load $KO || die "the A/B's xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XB.txt | head -3)"
say "kernel $(uname -r), $(grep -o 'rodata=[a-z]*' /proc/cmdline), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion)"
for tag in r74-3-loan64-B r74-4-loan64-A r74-4-loan64-B; do
	v=${tag##*-}
	load $tag
	echo $([ $v = B ] && echo Y || echo N) > $XL
	[ "$(cat $XL)" = "$([ $v = B ] && echo Y || echo N)" ] || die "$tag: rx_refill_lean $(cat $XL)"
	run $tag
	say "$tag | rx_refill_lean $(cat $XL)"
done
echo N > $XL
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
