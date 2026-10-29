#!/bin/bash
# run102.sh: round 42's diagnostic xeu, the receive poll's first touches.
# The diagnostic xeu (xeu-diag-poll-first-touches.patch on round 41's kept
# xeu, 9461DCE8A67161729215CE0) times, with poll_lat_every 16: the
# completion at a poll's head; on one frame in 16, the next frame's
# completion, software ring entry, buffer's struct page and first header
# line, each at its first touch, before the prefetch asks for it; on one
# page allocation in 16, page_pool_alloc_pages() and the struct page it
# returns; and every refill's doorbell and every re-arm.  One loan64 run at
# the current state (build 499, rodata=noalias, the tip's sunrpc, nfsd the
# kernel's), sampling for 8 s from 10 s into the timed load; then the kept
# xeu again and the settled setting.  Each xeu swap refuses to run with an
# NFS client connected.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r102
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/bc110c2e-b4d4-47f6-b9d0-baa760f5553e/scratchpad/xeu42diag/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=9461DCE8A67161729215CE0 XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters
say() { echo "$(date +%T) $*"; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh enp161s0f0 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; echo 0 > $P/poll_lat_every 2>/dev/null; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(modinfo -F srcversion $DIAG)" = $XD ] || die "$DIAG is not $XD"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XD, $(tail -1 $O/apply-$XD.txt)"
t=r102-diag-loan64
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo 0 > $P/poll_lat; w0=$(w); echo 16 > $P/poll_lat_every
  echo "poll_lat from $(date +%T)" > $O/look.when
  sleep 8; echo 0 > $P/poll_lat_every; w1=$(w)
  cat $P/poll_lat > $O/poll-lat.txt
  echo "WRITEs in poll_lat: $((w1 - w0))" >> $O/poll-lat.txt
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/poll_lat_every
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# Each kind over the ring CPUs: count, mean, and the shares at 64-127 ns and
# at 128 ns or more (a line from the L3 or beyond, and one from DRAM).
awk '/^WRITEs/ {w = $NF; next}
     / n / {k = $2; n[k] += $4; sum[k] += $6; for (b = 0; b < 16; b++) { if (b == 6) mid[k] += $(8 + b); if (b >= 7) slow[k] += $(8 + b) } }
     END { printf "WRITEs in the window %d\n", w
	   split("own cmpl_first cmpl_next sw_entry frame_page frame_hdr pp_alloc new_page doorbell rearm", ks, " ")
	   for (i = 1; i <= 10; i++) { k = ks[i]; if (n[k]) printf "%-11s n %9d mean %7.1f ns  64-127 ns %5.1f%%  >=128 ns %5.1f%%\n", k, n[k], sum[k] / n[k], 100 * mid[k] / n[k], 100 * slow[k] / n[k] } }' $O/poll-lat.txt | tee $O/poll-lat-sum.txt
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
