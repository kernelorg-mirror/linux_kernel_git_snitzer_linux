#!/bin/bash
# run72.sh: round 35, the third diagnostic.  run70.sh and run71.sh timed
# the refill's own accesses alone (the dma_addr load, the RX descriptor
# writes) and found them near, so the stall sampled in the refill is not
# theirs.  Is it memory work left from before the refill (the poll's
# prefetches and skb writes, GRO's flush) that the refill's first
# dependent access waits behind?  This xeu (E28076D19609AD19AD65BF7: the
# kept one with refill_lat) drains what is outstanding (DSB SY) at the
# start of every other refill and times the drain, and times every
# refill's loop with no barrier inside, those after a drain apart from
# the rest.  One loan64 run; then the kept xeu is loaded again and settled.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r72
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/9dd234f9-8d71-4758-ae14-f72417b58890/scratchpad/xeu35diag3/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=E28076D19609AD19AD65BF7 XK=DC23D10E104D370F14FB06E
LAT=/sys/module/xeu/parameters/refill_lat
say() { echo "$(date +%T) $*"; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh enp161s0f0 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() { say "STOPPED: $*"; [ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load; exit 1; }
client_free || die "client not free"
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
say "kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XD.txt)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r72-diag-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  echo 0 > $LAT; w0=$(w); sleep 4; cat $LAT > $O/refill-lat.txt; w1=$(w)
  echo "WRITEs in refill_lat: $((w1 - w0))" >> $O/refill-lat.txt
  ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
