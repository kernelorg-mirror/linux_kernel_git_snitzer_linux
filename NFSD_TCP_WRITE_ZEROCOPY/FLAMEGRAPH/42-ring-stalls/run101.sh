#!/bin/bash
# run101.sh: round 42's step 2, what the rings read beyond their L2, and
# where the card's writes go.  At the current state (build 499,
# rodata=noalias, the tip's sunrpc, nfsd the kernel's, xeu round 41's, the
# settled setting), no module swapped.
#  - The unit of bus_access_rd, from xeu-rig/busread.c on CPU 63 (port b's,
#    idle): 512 MiB read once, counted around the read loop.
#  - The mesh (arm_cmn_0, system-wide) at rest: four 3 s windows, one
#    counter group each, during the rest before the load.
#  - One loan64 run.  From 10 s into its timed load: six 3 s windows on the
#    ring CPUs (0-7) with rx_prefetch and rx_gro_frags switched in turn
#    (YY NY YN YY NY YN), 1 s to change, counting cycles, instructions,
#    bus_access_rd, l2d_cache_refill, ll_cache_miss_rd and
#    stall_backend_mem; then the mesh's four groups again under load.
#    settled.sh puts both switches back at the end.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r101
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XB=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters
BR=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/bc110c2e-b4d4-47f6-b9d0-baa760f5553e/scratchpad/busunit/busread
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; ./settled.sh apply > $O/apply-die.txt 2>&1; exit 1; }
grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(cat /sys/module/xeu/srcversion)" = $XB ] || die "xeu is not $XB"
[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "nfsd is not $N"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XB"
W0=$(dmesg | grep -c 'WARNING:')
C=("rnid_s0_wdata_beats rnid_s1_wdata_beats rnid_s2_wdata_beats rnid_s0_rdata_beats"
   "rnid_s1_rdata_beats rnid_s2_rdata_beats hnf_mc_reqs hnf_stash_data_pull"
   "hnf_slc_sf_cache_access hnf_cache_miss hnf_cache_fill hnf_slc_eviction"
   "hnf_snp_sent hnf_sf_hit hnf_sf_evictions hnf_dir_snoops_sent")
cmn() {	# cmn TAG: the mesh's four groups, 3 s each, with the WRITEs in each
	local i e ev w0 w1
	for i in 0 1 2 3; do
		ev=; for e in ${C[$i]}; do ev="$ev${ev:+,}arm_cmn_0/$e/"; done
		w0=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)
		perf stat -x, -a -e "$ev" -o $O/cmn-$1-$i.txt -- sleep 3
		w1=$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd)
		echo "WRITEs,$((w1 - w0))" >> $O/cmn-$1-$i.txt
	done
}
t=r101-loan64
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply-$t.txt 2>&1 || die "$t not settled: $(grep -E 'BAD|FAILED' $O/apply-$t.txt | head -3)"
taskset -c 63 $BR 512 | tee $O/busread.txt
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60
cmn rest
sleep 60
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 10 || { echo "the timed load did not start" > $O/look.when; exit 1; }
  echo "look from $(date +%T)" > $O/look.when
  i=0
  for s in YY NY YN YY NY YN; do
	i=$((i + 1)); echo ${s:0:1} > $P/rx_prefetch; echo ${s:1:1} > $P/rx_gro_frags; sleep 1
	w0=$(w); perf stat -x, -C 0-7 -e cycles,instructions,bus_access_rd,l2d_cache_refill,ll_cache_miss_rd,stall_backend_mem -o $O/ring-$i-$s.txt -- sleep 3; w1=$(w)
	echo "WRITEs,$((w1 - w0))" >> $O/ring-$i-$s.txt
  done
  echo Y > $P/rx_prefetch; echo Y > $P/rx_gro_frags
  echo "rings end $(date +%T)" >> $O/look.when
  cmn load
  echo "end $(date +%T)" >> $O/look.when ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=50 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1; rc=$?
wait
echo Y > $P/rx_prefetch; echo Y > $P/rx_gro_frags
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$t: a new WARNING in dmesg"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$t/geometry.txt) placed | $(./cost.sh $O/$t) | peak $((32768 - $(sort -n $O/$t/region-min-free.txt | head -1)))"
say "$(paste -sd ' ' $O/look.when)"
# The rings, a WRITE: lines read beyond L2 (two bus beats a line), L2
# refills, last-level read misses, memory stall cycles.
for f in $O/ring-*.txt; do
	awk -F, -v n=$(basename $f .txt) '$1 == "WRITEs" {w = $2} $3 == "cycles" {c = $1} $3 == "instructions" {i = $1} $3 == "bus_access_rd" {b = $1} $3 == "l2d_cache_refill" {r = $1} $3 == "ll_cache_miss_rd" {m = $1} $3 == "stall_backend_mem" {s = $1}
		END {printf "%-10s WRITEs %6d | a WRITE: cycles %.1fk instructions %.1fk lines read beyond L2 %.2fk L2 refills %.2fk LL read misses %.2fk memory stalls %.1fk\n", n, w, c / w / 1000, i / w / 1000, b / 2 / w / 1000, r / w / 1000, m / w / 1000, s / w / 1000}' $f
done | tee $O/ring-sum.txt
# The mesh: each count a second at rest and under load, and a WRITE under
# load less the rest.
for i in 0 1 2 3; do
	paste -d, $O/cmn-rest-$i.txt $O/cmn-load-$i.txt | awk -F, -v SECS=3 '
		$1 == "WRITEs" {next} $3 ~ /arm_cmn/ {name[++k] = $3; r[k] = $1; l[k] = $(NF/2 + 1)}
		END {for (j = 1; j <= k; j++) printf "%s %d %d\n", name[j], r[j], l[j]}' > $O/cmn-$i.pair
	w=$(awk -F, '$1 == "WRITEs" {print $2}' $O/cmn-load-$i.txt)
	awk -v w=$w '{gsub("arm_cmn_0/|/", "", $1); printf "%-26s rest %8.1fM/s load %8.1fM/s | a WRITE, less the rest: %8.1f\n", $1, $2 / 3e6, $3 / 3e6, ($3 - $2) / w}' $O/cmn-$i.pair
done | tee $O/cmn-sum.txt
./settled.sh apply > $O/apply-end.txt 2>&1
say "end: $(tail -1 $O/apply-end.txt); DONE"
