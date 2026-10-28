#!/bin/bash
# run98.sh: round 41's step 3, the A/B for handing GRO page fragments.  One
# xeu, CA0931345E46C929B11D01F (round 35's kept xeu with
# xeu-ab-gro-frags.patch: rx_gro_frags, switched between runs without a
# reload), at the current state (build 499, rodata=noalias, the tip's
# sunrpc, nfsd the kernel's, the settled setting).  Eight loan64 runs,
# counted and untraced, rx_gro_frags N Y Y N N Y Y N.  The kept xeu is put
# back at the end.  Each xeu swap refuses to run with an NFS client
# connected.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r98
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
AB=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/bc110c2e-b4d4-47f6-b9d0-baa760f5553e/scratchpad/xeu41frags/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XA=CA0931345E46C929B11D01F XK=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters
say() { echo "$(date +%T) $*"; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh enp161s0f0 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
load() {	# load TAG: the settled setting, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XA ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XA"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(modinfo -F srcversion $AB)" = $XA ] || die "$AB is not $XA"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
xeu_load $AB || die "the A/B's xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XA.txt | head -3)"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XA.txt)"
i=0
for f in N Y Y N N Y Y N; do
	i=$((i + 1)); tag=r98-$i-loan64-$f
	load $tag
	echo $f > $P/rx_gro_frags
	[ "$(cat $P/rx_gro_frags)" = $f ] || die "$tag: rx_gro_frags $(cat $P/rx_gro_frags)"
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=64 ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	[ $rc = 0 ] || { echo N > $P/rx_gro_frags; die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"; }
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt) | rx_gro_frags $(cat $P/rx_gro_frags)"
done
echo N > $P/rx_gro_frags
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
