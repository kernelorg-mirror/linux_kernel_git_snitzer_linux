#!/bin/bash
# placement.sh <n>...: FLAMEGRAPH round 155, how the rtc balance places
# connections on nfsd's CPUs (Mike, 19 November: "do add that snapshot and
# run it with no spill as round 155").  Round 154's t2-r1214 ran 10% below
# t2-r1212 with ten of nfsd's CPUs 69 to 83% busy while others were full:
# the connections placed unevenly?  Each <n> is one IOPS run, round 151's
# packets.sh on the IOPS setting (no spill since round 154), and through
# it, from the load's first second, nfsd's rtc_stats every second
# (debugfs: "placed cpu:n", each CPU's connections, and "rebalanced",
# moves so far) into $L/t2-r<n>/rtc-placed.txt.  placement.awk turns it
# into each second's spread over nfsd's CPUs 8-55 and, over the mpstat
# window, each CPU's mean connections beside its busy.  Only reads.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541; P=../FLAMEGRAPH/155-rtc-placement
say() { echo "$(date +%T) $*"; }
for n in "$@"; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
for n in "$@"; do
	./step.sh $G pk-$n ../FLAMEGRAPH/151-packets/packets.sh $n > $G/t2-r$n.log 2>&1 & pid=$!
	W=$L/t2-r$n/run/wire-sec.tsv
	until [ -f $W ] || ! kill -0 $pid 2>/dev/null; do sleep 0.2; done
	while kill -0 $pid 2>/dev/null && [ $(wc -l < $W 2>/dev/null || echo 0) -lt 30 ]; do
		echo "t $(date +%s.%N) $(wc -l < $W)"; cat /sys/kernel/debug/nfsd/rtc_stats | grep -E "^(placed|rebalanced)"
		sleep 1
	done > $L/t2-r$n/rtc-placed.txt
	wait $pid
	line=$(grep -E "t2-r$n " $G/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s: server %.2f us, client %.2f us of CPU a READ\n", n, s / 100 * 64 / m, c / 100 * 48 / m }'
	grep -E "^ring rx" $L/t2-r$n/packets.txt | sed 's/^/    /'
	awk -f $P/placement.awk $L/t2-r$n/rtc-placed.txt $L/t2-r$n/run/srv-mpstat.txt | tee $L/t2-r$n/placement.txt | sed 's/^/    /'
done
say "placement all done"
