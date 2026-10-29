#!/bin/bash
# run100.sh: round 42's step 1, the rings' stalls counted.  At the current
# state (build 499, rodata=noalias, the tip's sunrpc, nfsd the kernel's,
# xeu round 41's with rx_gro_frags Y, the settled setting), no module
# swapped: one loan64 run.  From 10 s into its timed load, five 3 s
# perf stat windows on the ring CPUs (0-7), one counter group each with
# cycles and instructions, then 3 s of bpftrace for frames a ring poll.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r100
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XB"
W0=$(dmesg | grep -c 'WARNING:')
t=r100-stat-loan64
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply-$t.txt 2>&1 || die "$t not settled: $(grep -E 'BAD|FAILED' $O/apply-$t.txt | head -3)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
G=("stall_frontend,stall_backend,stall_backend_mem,stall_slot_backend"
   "dsb_spec,dmb_spec,isb_spec,exc_irq"
   "l1d_cache_refill,l2d_cache_refill,l3d_cache_refill,ll_cache_miss_rd"
   "bus_access_rd,bus_access_wr,remote_access,dtlb_walk"
   "napi:napi_poll,irq:irq_handler_entry")
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "look from $(date +%T)" > $O/look.when
  for i in 0 1 2 3 4; do
	w0=$(w); perf stat -x, -C 0-7 -e cycles,instructions,${G[$i]} -o $O/stat-$i.txt -- sleep 3; w1=$(w)
	echo "WRITEs,$((w1 - w0))" >> $O/stat-$i.txt; sleep 1
  done
  w0=$(w)
  bpftrace -e 'tracepoint:napi:napi_poll /cpu < 8/ { @frames = lhist(args.work, 0, 72, 4); @polls = count(); @sum = sum(args.work); @at_budget = sum(args.work == args.budget ? 1 : 0); } interval:s:3 { exit(); }' > $O/polls.txt 2>&1
  w1=$(w); echo "WRITEs in the window: $((w1 - w0))" >> $O/polls.txt
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# Each counter a WRITE, and as a share of its window's cycles.
for i in 0 1 2 3 4; do
	awk -F, -v i=$i '$1 == "WRITEs" {w = $2; next} $3 != "" && $1 ~ /^[0-9]+$/ {n[++k] = $3; v[k] = $1; if ($3 == "cycles") c = $1}
		END {printf "window %d (%d WRITEs):", i, w; for (j = 1; j <= k; j++) printf " %s %.1fk%s", n[j], v[j] / w / 1000, (n[j] ~ /^stall/ ? sprintf(" (%.1f%%)", 100 * v[j] / c) : ""); printf "\n"}' $O/stat-$i.txt
done | tee $O/stat-sum.txt
grep -E '^@(polls|sum|at_budget)|^WRITEs' $O/polls.txt | paste -sd ' '
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tail -1 $O/apply-end.txt); DONE"
