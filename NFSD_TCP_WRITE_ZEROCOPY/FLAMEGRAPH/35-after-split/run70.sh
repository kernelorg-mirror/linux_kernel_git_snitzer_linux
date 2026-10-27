#!/bin/bash
# run70.sh: round 35, how long the refill's load of a page's dma_addr
# waits, now that round 34 took page_pool's frag drain out of the refill,
# and who freed the pages it loads.  A diagnostic xeu (A249DE896A5D41390757A2C:
# the kept one, DC23D10E104D370F14FB06E, with refill_lat) is loaded for one
# loan64 run from outside updates/, with /etc/modprobe.d/xeu.conf's options;
# from 5 s after the WRITEs pass 10,000 a second: 4 s of refill_lat, then
# 4 s of bpftrace counting the page-pool frees by CPU.  Then the kept xeu
# is loaded again and settled.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r70
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/9dd234f9-8d71-4758-ae14-f72417b58890/scratchpad/xeu35diag/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=A249DE896A5D41390757A2C XK=DC23D10E104D370F14FB06E
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
t=r70-diag-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  echo 0 > $LAT; w0=$(w); sleep 4; cat $LAT > $O/refill-lat.txt; w1=$(w)
  echo "WRITEs in refill_lat: $((w1 - w0))" >> $O/refill-lat.txt
  w0=$(w)
  timeout -s INT 4 bpftrace -e '
	kprobe:napi_pp_put_page { @put[cpu < 8 ? "rings" : "nfsd", "napi_pp_put_page"] = count(); }
	kprobe:page_pool_put_netmem_bulk { @put[cpu < 8 ? "rings" : "nfsd", "page_pool_put_netmem_bulk"] = count(); @bulk_pages[cpu < 8 ? "rings" : "nfsd"] = sum(arg1); }
	kprobe:page_pool_put_unrefed_netmem { @put[cpu < 8 ? "rings" : "nfsd", "page_pool_put_unrefed_netmem"] = count(); }' > $O/frees.txt 2>&1 < /dev/null
  w1=$(w); echo "WRITEs in the count: $((w1 - w0))" >> $O/frees.txt ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
xeu_load || die "kept xeu not loaded"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
say "end: xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-$XK.txt); DONE"
