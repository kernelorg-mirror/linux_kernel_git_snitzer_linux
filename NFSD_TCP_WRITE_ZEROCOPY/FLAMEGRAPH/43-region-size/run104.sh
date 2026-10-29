#!/bin/bash
# run104.sh: the receive region at four sizes, 32768 pages (512 MiB, the
# settled size), 6400 (100 MiB), 4096 (64 MiB) and 2048 (32 MiB), then
# 32768 again.  At each, one copy run and one loan run whose budget fits
# the region with 1024 pages (16 MiB) left for the rings and the copy:
# 64 WRITEs at 32768 (loan64, as every round runs), 41 at 6400, 23 at 4096
# and 7 at 2048.  Round 41's kept xeu, build 499, rodata=noalias, the tip's
# sunrpc, nfsd the kernel's, the settled setting but for the region's size
# (EXP_REGION_PAGES).  Counted and untraced, WARM=10 RUNTIME=30.
#
# The region is resized as page_pool's comment says: the new size is
# written, the port goes down so that its pools go and the old region is
# given back, and the port comes up again, making the new one.  Each resize
# refuses to run with an NFS client connected, waits for the old region to
# go (debugfs) before the port comes up, and checks the new one was made.
# This is the first run of the resize on the rig.  If 32768 contiguous
# pages cannot be had again at the end, the rig is left at the size it
# could make, STOPPED says so, and a reboot puts the region back.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r104
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
PA=enp161s0f0
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
resize() {	# resize PAGES: the region at PAGES, port a back up and settled
	local r=$1 t
	[ "$(reg pages)" = $r ] && return 0
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "resize to $r: an NFS client is connected"
	client_free || die "resize to $r: client not free"
	echo $r > $PP/rx_region_pages
	( sleep 300; ip link set $PA up ) > /dev/null 2>&1 & local dm=$!
	ip link set $PA down
	for t in $(seq 120); do [ -e $RG ] || break; sleep 1; done
	[ -e $RG ] && { ip link set $PA up; kill $dm; die "resize to $r: the old region was not given back in 120 s: $(tr '\n' ' ' < $RG)"; }
	ip link set $PA up; sleep 6; kill $dm 2>/dev/null
	./port-setup.sh $PA 0 > $O/port-setup-$r.txt 2>&1
	export EXP_REGION_PAGES=$r
	./settled.sh apply > $O/apply-region-$r.txt 2>&1 || die "region $r not settled: $(grep -E 'BAD|FAILED' $O/apply-region-$r.txt | head -3)"
	[ "$(reg pages)" = $r ] || die "region $r not made: $(dmesg | grep 'page_pool:' | tail -2)"
	say "region $r pages ($((r / 64)) MiB) made at $(reg phys_base); $(dmesg | grep 'rx region for .* freed' | tail -1 | sed 's/^\[[^]]*\] //')"
}
load() {	# load TAG: the settled setting at this size, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
run() {	# run TAG WRITES: one run, copy when WRITES is 0
	local tag=$1 w=$2 rc f0 f1 lw=
	[ $w -gt 0 ] && lw="WRITES=$w"
	load $tag
	f0=$(reg alloc_fail)
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag $lw ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | in use at most $(sed -n 's/^region pages in use at most: //p' $O/$tag/summary.txt) of $(reg pages) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt || echo copy) | $(grep -o 'client retransmits.*' $O/$tag/window.txt)"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
[ "$(cat $PP/rx_region_dev)" = 0000:a1:00.0 ] || die "region devices $(cat $PP/rx_region_dev), not port a's alone"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion)"
i=0
for s in 32768:64 6400:41 4096:23 2048:7 32768:64; do
	r=${s%:*} w=${s#*:}
	resize $r
	i=$((i + 1))
	run r104-$i-region$r-copy 0
	run r104-$i-region$r-loan$w $w
done
unset EXP_REGION_PAGES
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "end: region $(reg pages) pages, xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-end.txt); DONE"
