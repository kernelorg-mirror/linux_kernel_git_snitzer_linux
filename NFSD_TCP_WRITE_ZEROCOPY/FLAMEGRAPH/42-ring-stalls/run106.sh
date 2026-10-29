#!/bin/bash
# run106.sh: round 42's step 4, the receive polls that find nothing.  The
# diagnostic xeu (xeu-diag-empty-polls.patch on round 41's kept xeu,
# 4A7F5BFDEEE8F39ACF263F6), with empty_lat_on: on every receive poll, the
# ring's interrupt to its poll; the ring's last unmask to the interrupt,
# for polls that find the head empty and those that find work; on every
# other empty poll a spin of up to 4 us for the head to turn valid, and on
# the others the empty poll's time, from its start and from the interrupt;
# and counts of polls by origin, of kicks NAPI had already taken, of
# budgets used, of napi_complete_done() returning false and of unmasks.
# The interrupts and softirqs on CPUs 0-7 over the same window.  One
# loan64 run at the current state (build 499, rodata=noalias, the tip's
# sunrpc, nfsd the kernel's, the region at 32768), sampling for 8 s from
# 10 s into the timed load; then the kept xeu again and the settled setting.
# Each xeu swap refuses to run with an NFS client connected.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r106
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/b565c22f-4f2e-47c6-9d6c-f73fc2c0b4a9/scratchpad/xeu42empty/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=4A7F5BFDEEE8F39ACF263F6 XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
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
die() { say "STOPPED: $*"; echo 0 > $P/empty_lat_on 2>/dev/null; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is not the kept $XK"
[ "$(modinfo -F srcversion $DIAG)" = $XD ] || die "$DIAG is not $XD"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XD, region $(reg pages), $(tail -1 $O/apply-$XD.txt)"
t=r106-diag-loan64
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo 0 > $P/empty_lat; w0=$(w); cat /proc/interrupts > $O/irq0.txt; cat /proc/softirqs > $O/softirq0.txt; echo 1 > $P/empty_lat_on
  echo "empty_lat from $(date +%T)" > $O/look.when
  sleep 8; echo 0 > $P/empty_lat_on; w1=$(w); cat /proc/interrupts > $O/irq1.txt; cat /proc/softirqs > $O/softirq1.txt
  cat $P/empty_lat > $O/empty-lat.txt
  echo "WRITEs in empty_lat: $((w1 - w0))" >> $O/empty-lat.txt
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo 0 > $P/empty_lat_on
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# The counts and each kind over the ring CPUs: count, a WRITE, mean, and
# the shares by time.
awk '/^WRITEs/ {w = $NF; next}
     $2 == "counts" {for (i = 3; i < NF; i += 2) { if (!($i in c)) cs[++nc] = $i; c[$i] += $(i + 1) }; next}
     / n / {k = $2; if (!(k in n)) ks[++nk] = k; n[k] += $4; sum[k] += $6
	    for (b = 0; b < 16; b++) { x = $(8 + b); j = b <= 6 ? 0 : b >= 13 ? 7 : b - 6; h[k, j] += x } }
     END { printf "WRITEs in the window %d\n", w
	   for (i = 1; i <= nc; i++) printf "%-24s %10d %8.2f a WRITE\n", cs[i], c[cs[i]], c[cs[i]] / w
	   printf "%-31s %9s %6s %8s  %6s %6s %6s %6s %6s %6s %6s %6s\n", "", "n", "/W", "mean", "<128", "128-", "256-", "512-", "1us-", "2us-", "4us-", ">=8us"
	   for (i = 1; i <= nk; i++) { k = ks[i]; printf "%-31s %9d %6.2f %8.1f ", k, n[k], n[k] / w, sum[k] / n[k]
	     for (j = 0; j <= 7; j++) printf " %5.1f%%", 100 * h[k, j] / n[k]; printf "\n" } }' $O/empty-lat.txt | tee $O/empty-lat-sum.txt
# Interrupts and softirqs on CPUs 0-7 in the window, a WRITE (the RX rings
# are on 0, 2, 4 and 6, the TX rings on 1, 3, 5 and 7).
w=$(awk '/^WRITEs/ {print $NF}' $O/empty-lat.txt)
for f in irq softirq; do
	awk -v w=$w 'FNR == 1 {next} NR == FNR {a[$1] = $0; next}
	     ($1 in a) {split(a[$1], o); s = 0; line = ""; for (c = 2; c <= 9; c++) { d = $c - o[c]; s += d; line = line sprintf(" %8.2f", d / w) }
			if (s > 0) printf "%-12s%s  %s\n", $1, line, ($NF ~ /^[0-9]+$/ ? "" : $NF) }' $O/${f}0.txt $O/${f}1.txt
done | tee $O/irq-sum.txt
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), region $(reg pages), $(tail -1 $O/apply-$XK.txt); DONE"
