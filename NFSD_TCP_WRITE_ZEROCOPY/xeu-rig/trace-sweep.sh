#!/bin/bash
# trace-sweep.sh <outdir> <plan> - sweep.sh, with these run inside every
# cell from 35 s in: where.sh (both ends' CPUs, the rings, a profile), the
# page pools' counters over the same 10 s, perf lock contention by caller
# (10 s), a profile with call graphs (8 s, left as cg-*.data for perf
# report), nfsdlat.bt (15 s: the histograms of an nfsd
# WRITE and who takes the region's lock) and handoff.bt (8 s: how many
# calls a WRITE's record is received in and what it waits for) and
# linger.bt (8 s: how long each linger wait lasted and how it ended;
# empty with the linger off).  A traced cell's rate is a traced
# rate.  The environment is passed on to sweep.sh.
cd "$(dirname "$0")"
. ./common.sh
O=${1:?outdir}; P=${2:?plan}
./sweep.sh $O $P > $O.log 2>&1 &
SW=$!
seen=" "
pp() { ethtool -S $D | grep -E 'rx_pp_|rx_packets'; }
while kill -0 $SW 2>/dev/null; do
	for f in $O/*/stress-A/delay-steps.txt; do
		[ -e "$f" ] || continue
		c=$(awk '/fio start/ && $2 != "warmup" {n = $2} END {print n}' $f)
		[ -n "$c" ] || continue
		k="$f:$c"
		case "$seen" in *" $k "*) continue ;; esac
		seen="$seen$k "
		t=$(basename $(dirname $(dirname $f)))
		sleep 35
		p0=$(pp)
		./where.sh $O/where-$t-$c 10 > $O/where-$t-$c.txt 2>&1
		paste <(echo "$p0") <(pp) | awk '{printf "%s %d\n", $1, $4-$2}' > $O/pp-$t-$c.txt
		perf lock contention -a -b -E 30 -- sleep 10 > $O/lock-caller-$t-$c.txt 2>&1
		perf record -a -g -F 99 -o $O/cg-$t-$c.data -- sleep 8 > /dev/null 2>&1
		timeout -s INT 15 bpftrace nfsdlat.bt > $O/nfsdlat-$t-$c.txt 2>&1
		timeout -s INT 8 bpftrace handoff.bt > $O/handoff-$t-$c.txt 2>&1
		timeout -s INT 8 bpftrace linger.bt > $O/linger-$t-$c.txt 2>&1
	done
	sleep 1
done
wait $SW
