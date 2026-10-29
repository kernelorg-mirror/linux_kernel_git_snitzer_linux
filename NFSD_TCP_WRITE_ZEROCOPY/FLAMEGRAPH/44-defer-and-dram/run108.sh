#!/bin/bash
# run108.sh: round 44's step 2, where the rings' reads from DRAM are made.
# Round 41's kept xeu at the settled setting (build 499, rodata=noalias,
# the tip's sunrpc, nfsd the kernel's, region 32768), no module swapped:
# one loan64 run (WARM=10 RUNTIME=40).  From 10 s into its timed load:
#   1. per CPU on 0-7 (RX rings on 0, 2, 4, 6, TX rings on 1, 3, 5, 7),
#      counted: two 3 s windows of cycles and instructions with
#        ll_cache_miss_rd, l2d_cache_refill, l2d_cache_lmiss_rd, l1d_cache_refill
#        stall_backend_mem, bus_access_rd, mem_access_rd, l3d_cache_refill
#   2. 5 s sampling ll_cache_miss_rd (one in 5000) on CPUs 0-7 with call
#      chains, at the settled setting;
#   3. the same with xeu's rx_prefetch N, so that the next frame's prefetch
#      is not outstanding (and its misses sampled) under GRO's frames.
# settled.sh puts rx_prefetch back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r108
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; echo Y > $P/rx_prefetch; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is not $XK"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
[ "$(awk '$1 == "pages" {print $2}' /sys/kernel/debug/page_pool_rx_region)" = 32768 ] || die "the region is not 32768 pages"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XK"
W0=$(dmesg | grep -c 'WARNING:')
t=r108-dram-loan64
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply-$t.txt 2>&1 || die "$t not settled: $(grep -E 'BAD|FAILED' $O/apply-$t.txt | head -3)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
G=("ll_cache_miss_rd,l2d_cache_refill,l2d_cache_lmiss_rd,l1d_cache_refill"
   "stall_backend_mem,bus_access_rd,mem_access_rd,l3d_cache_refill")
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "look from $(date +%T)" > $O/look.when
  for i in 0 1; do
	w0=$(w); perf stat -x, -A -C 0-7 -e cycles,instructions,${G[$i]} -o $O/stat-$i.txt -- sleep 3; w1=$(w)
	echo "WRITEs,$((w1 - w0))" >> $O/stat-$i.txt; sleep 1
  done
  w0=$(w); perf record -C 0-7 -g -e ll_cache_miss_rd -c 5000 -o $O/miss-pf.data -- sleep 5 > /dev/null 2>&1; w1=$(w)
  echo "WRITEs in miss-pf: $((w1 - w0))" >> $O/look.when
  echo N > $P/rx_prefetch; sleep 1
  w0=$(w); perf record -C 0-7 -g -e ll_cache_miss_rd -c 5000 -o $O/miss-nopf.data -- sleep 5 > /dev/null 2>&1; w1=$(w)
  echo Y > $P/rx_prefetch
  echo "WRITEs in miss-nopf: $((w1 - w0))" >> $O/look.when
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo Y > $P/rx_prefetch
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# Each counter a WRITE, for the RX CPUs and the TX CPUs apart.
for i in 0 1; do
	awk -F, '$1 == "WRITEs" {w = $2; next}
		 $1 ~ /^CPU[0-7]$/ && $2 ~ /^[0-9]+$/ {c = substr($1, 4) + 0; g = c % 2 ? "tx" : "rx"; if (!(($4) in seen)) { seen[$4]; ev[++k] = $4 } v[g, $4] += $2}
		 END {for (j = 1; j <= k; j++) printf "%-20s rx %8.1fk  tx %8.1fk a WRITE\n", ev[j], v["rx", ev[j]] / w / 1000, v["tx", ev[j]] / w / 1000}' $O/stat-$i.txt
done | tee $O/stat-sum.txt
# The samples by symbol, self, for each group of CPUs and each half.
for d in pf nopf; do
	for g in rx:0,2,4,6 tx:1,3,5,7; do
		perf report -i $O/miss-$d.data --cpu ${g#*:} --no-children --no-inline -g none --percent-limit 0.5 --sort symbol 2> /dev/null | grep -v '^$' | head -60 > $O/miss-$d-${g%%:*}.txt
		perf report -i $O/miss-$d.data --cpu ${g#*:} --no-children --no-inline -g caller,0.5,callee,function,percent --percent-limit 2 --sort symbol 2> /dev/null | grep -v '^$' | head -400 > $O/miss-$d-${g%%:*}-callers.txt
	done
done
for f in $O/miss-{pf,nopf}-{rx,tx}.txt; do echo "== ${f##*/}"; grep -E '^ +[0-9]' $f | head -15; done
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: rx_prefetch $(cat $P/rx_prefetch), $(tail -1 $O/apply-end.txt); DONE"
