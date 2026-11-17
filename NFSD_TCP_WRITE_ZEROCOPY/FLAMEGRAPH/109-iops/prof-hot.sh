#!/bin/bash
# prof-hot.sh <n> <iodepth> <nconnect>: FLAMEGRAPH round 109, what the
# server's busiest CPU does (Mike, 17 November: "profile the hot CPU
# after the A/B").  One nfs4k.sh run under xeu-rig/settled-iops.sh, and
# from the load's second 8, beside nfs4k.sh's 10 s of mpstat:
#   8 s of perf record -a -g at 999 Hz on the server (into /dev/shm,
#   removed after);
#   rtc_stats' "placed" line (rtc connections on each work CPU) at the
#   start and the end of those 8 s.
# Then the busiest of nfsd's work CPUs (8-55) by mpstat, and for it and
# for the work CPU at the median: its samples by command, by symbol
# with and without children; where nfsdcld and systemd-journald ran;
# every CPU's share of the samples.  Into $L/t2-r<n>/hot-*.txt.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-hot.sh <n> <iodepth> <nconnect>}; qd=${2:?iodepth}; nc=${3:?nconnect}
O=$L/t2-r$n; mkdir -p $O || exit 1
D=/dev/shm/zc-p109.data
row() { W=$O/run/wire-sec.tsv; for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge $1 ] && return 0; sleep 0.2; done; return 1; }
(
	row 8 || exit 0
	grep '^placed' /sys/kernel/debug/nfsd/rtc_stats > $O/hot-placed.txt
	perf record -q -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
	grep '^placed' /sys/kernel/debug/nfsd/rtc_stats >> $O/hot-placed.txt
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$qd:$nc; rc=$?
wait
[ -s $D ] || { echo "no perf data"; exit 1; }
busy=$(awk '$1 == "Average:" && $2 == "CPU" {for (i = 1; i <= NF; i++) c[$i] = i}
	$1 == "Average:" && $2 ~ /^[0-9]+$/ && $2 >= 8 && $2 <= 55 {printf "%s %.1f\n", $2, 100 - $c["%idle"] - $c["%iowait"]}' $O/srv-mpstat-w.txt | sort -k2 -nr)
hot=$(echo "$busy" | awk 'NR == 1 {print $1}'); mid=$(echo "$busy" | awk 'NR == 24 {print $1}')
echo "$busy" > $O/hot-busy.txt
R="perf report -i $D"
for w in hot:$hot mid:$mid; do
	t=${w%%:*}; c=${w#*:}
	$R --cpu $c --no-children -g none --sort comm --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -40 > $O/hot-$t-comm.txt
	$R --cpu $c --no-children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -80 > $O/hot-$t-self.txt
	$R --cpu $c --children -g none --sort sym --percent-limit 1 2>/dev/null | grep -v '^#' | grep . | head -120 > $O/hot-$t-children.txt
done
$R --comm nfsdcld,systemd-journal --no-children -g none --sort comm,cpu 2>/dev/null | grep -v '^#' | grep . | head -40 > $O/hot-housekeeping.txt
$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/hot-cpu.txt
rm -f $D
pl() { awk -v c=$1 'NR == 1 {for (i = 2; i <= NF; i++) {split($i, a, ":"); if (a[1] == c) {print a[2]; exit}}}' $O/hot-placed.txt; }
echo "busiest work CPU $hot ($(echo "$busy" | head -1 | cut -d' ' -f2)% busy, $(pl $hot) rtc connections placed), median $mid ($(echo "$busy" | sed -n 24p | cut -d' ' -f2)%, $(pl $mid))"
for t in hot mid; do echo "== $t: by command"; head -8 $O/hot-$t-comm.txt; echo "== $t: top self"; head -15 $O/hot-$t-self.txt; done
echo "== nfsdcld and systemd-journald, by CPU"; head -12 $O/hot-housekeeping.txt
exit $rc
