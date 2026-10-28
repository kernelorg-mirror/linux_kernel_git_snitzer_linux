#!/bin/bash
# run97.sh: round 41's step 2, where GRO's misses are.  The diagnostic xeu
# (xeu-diag-gro-lines.patch on round 35's kept xeu, FD970ED52080A4FA32D33AE)
# times, on one frame in 16 just before napi_gro_receive(), the lines GRO's
# first touches load: the frame's second header line, the held aggregate's
# cb, header line and shared info, and its last skb's shared info, beside a
# line of the CPU's own.  One loan64 run at the current state (build 499,
# rodata=noalias, the tip's sunrpc, nfsd the kernel's), sampling on for 8 s
# from 10 s into the timed load; then the kept xeu is loaded again and
# settled.  The xeu swap drops no NFS client: it refuses to run with one
# connected.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r97
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/bc110c2e-b4d4-47f6-b9d0-baa760f5553e/scratchpad/xeu41diag/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=FD970ED52080A4FA32D33AE XK=63631721F8B149913C0DC19 N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters
say() { echo "$(date +%T) $*"; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh enp161s0f0 0 > $O/port-setup-$(cat $P/../srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat $P/../srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(modinfo -F srcversion $DIAG)" = $XD ] || die "$DIAG is not $XD"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XD, $(tail -1 $O/apply-$XD.txt)"
t=r97-diag-loan64
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo 0 > $P/gro_lat; w0=$(w); echo 16 > $P/gro_lat_every
  echo "gro_lat from $(date +%T)" > $O/look.when
  sleep 8; echo 0 > $P/gro_lat_every; w1=$(w)
  cat $P/gro_lat > $O/gro-lat.txt
  echo "WRITEs in gro_lat: $((w1 - w0))" >> $O/gro-lat.txt
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/gro_lat_every
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# Each line over the ring CPUs: loads, mean ns, and the share at 128 ns or more.
awk '/sampled/ {s += $3; no += $5; ot += $7; next}
     / n / {k = $2; n[k] += $4; sum[k] += $6; for (b = 0; b < 16; b++) { h[k, b] += $(8 + b); if (b >= 7) slow[k] += $(8 + b) } }
     END { printf "sampled %d, no aggregate held %d, another flow'"'"'s %d\n", s, no, ot
	   split("own hdr2 agg_cb agg_hdr agg_shinfo last_shinfo", ks, " ")
	   for (i = 1; i <= 6; i++) { k = ks[i]; if (n[k]) printf "%-12s n %9d mean %6.1f ns  >=128 ns %5.1f%%\n", k, n[k], sum[k] / n[k], 100 * slow[k] / n[k] } }' $O/gro-lat.txt | tee $O/gro-lat-sum.txt
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
