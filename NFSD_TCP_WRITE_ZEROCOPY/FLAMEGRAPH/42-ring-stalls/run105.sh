#!/bin/bash
# run105.sh: round 42's step 3, second item: the completion at a receive
# poll's head.  The diagnostic xeu (xeu-diag-poll-head.patch on round 41's
# kept xeu, EDAEF51052A598878DAED83) times, on every poll while head_lat_on
# is set: a line of the CPU's own (the floor), the completion before the
# head (the control: read by the last poll, not written by the card since),
# the head's completion, and the same line again straight after; split by
# whether the poll follows the ring's interrupt or not, and by whether
# interrupts are on during the reads (every other poll has them off).  It
# also times the ring's interrupt to its poll, and counts empty heads.  One
# loan64 run at the current state (build 499, rodata=noalias, the tip's
# sunrpc, nfsd the kernel's), sampling for 8 s from 10 s into the timed
# load; then the kept xeu again and the settled setting.
#
# run104 (round 43) was stopped on 29 October with the region at 6400
# pages; the region is first put back to the settled 32768 the way run104
# resizes it.  Each xeu swap and the resize refuse to run with an NFS client
# connected.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r105
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/b565c22f-4f2e-47c6-9d6c-f73fc2c0b4a9/scratchpad/xeu42cmpl/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=EDAEF51052A598878DAED83 XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters PA=enp161s0f0
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh $PA 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; echo 0 > $P/head_lat_on 2>/dev/null; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
resize() {	# resize PAGES: as run104's
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
	./settled.sh apply > $O/apply-region-$r.txt 2>&1 || die "region $r not settled: $(grep -E 'BAD|FAILED' $O/apply-region-$r.txt | head -3)"
	[ "$(reg pages)" = $r ] || die "region $r not made: $(dmesg | grep 'page_pool:' | tail -2)"
	say "region $r pages ($((r / 64)) MiB) made at $(reg phys_base); $(dmesg | grep 'rx region for .* freed' | tail -1 | sed 's/^\[[^]]*\] //')"
}
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is not the kept $XK"
[ "$(modinfo -F srcversion $DIAG)" = $XD ] || die "$DIAG is not $XD"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
resize 32768
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XD, region $(reg pages), $(tail -1 $O/apply-$XD.txt)"
t=r105-diag-loan64
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo 0 > $P/head_lat; w0=$(w); echo 1 > $P/head_lat_on
  echo "head_lat from $(date +%T)" > $O/look.when
  sleep 8; echo 0 > $P/head_lat_on; w1=$(w)
  cat $P/head_lat > $O/head-lat.txt
  echo "WRITEs in head_lat: $((w1 - w0))" >> $O/head-lat.txt
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/head_lat_on
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# Each kind over the ring CPUs: count, a WRITE, mean, and the shares by time
# (32-63 ns the floor, 64-127 the L3, 128-255 DRAM, then 256-511, 512-1023
# and 1 us or more).
awk '/^WRITEs/ {w = $NF; next}
     / n / {k = $2; if (!(k in n)) ks[++nk] = k; n[k] += $4; sum[k] += $6
	    for (b = 0; b < 16; b++) { x = $(8 + b); if (b <= 5) c[k, 0] += x; else if (b <= 9) c[k, b - 5] += x; else c[k, 5] += x } }
     END { printf "WRITEs in the window %d\n", w
	   printf "%-20s %9s %6s %7s  %6s %6s %6s %6s %6s %6s\n", "", "n", "/W", "mean", "<64", "64-", "128-", "256-", "512-", ">=1us"
	   for (i = 1; i <= nk; i++) { k = ks[i]; printf "%-20s %9d %6.2f %7.1f ", k, n[k], n[k] / w, sum[k] / n[k]
	     for (j = 0; j <= 5; j++) printf " %5.1f%%", 100 * c[k, j] / n[k]; printf "\n" } }' $O/head-lat.txt | tee $O/head-lat-sum.txt
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), region $(reg pages), $(tail -1 $O/apply-$XK.txt); DONE"
