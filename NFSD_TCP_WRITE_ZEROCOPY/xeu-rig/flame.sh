#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# flame.sh - one one-port loan run with a profile in its window, for a round
# of FLAMEGRAPH/README.md (analyze, fix, test, repeat).
#
#   flame.sh OUTDIR TAG [WRITES]     run, record, then report
#   WHICH=ab flame.sh OUTDIR TAG ... both ports at the two-port setting, each
#                                  onto its own four drives; the rings are
#                                  both ports' Rx CPUs (0,2,4,6,56,58,60,62)
#   flame.sh report DIR              report again from DIR/perf.data
#
# The run is the one-port battery's (port a, the four drives /hs/nvme4-7,
# WARM=10 RUNTIME=30, inside the drives' fast region), after a trim and
# 120 s of rest, WRITES=64 by default (0 is copy mode).  When twoport.sh opens
# its 10 s window (it starts srv-mpstat.txt), perf records every CPU for 8 s
# at 999 Hz with kernel call chains.  Then, in OUTDIR/TAG-flame/:
#   rings.svg, nfsd.svg, all.svg   flamegraphs (Brendan Gregg's FlameGraph, in
#                                  $FG, default /opt/FlameGraph) of port a's
#                                  ring CPUs (0,2,4,6), nfsd's (8-55) and all,
#                                  weighted by cycles, idle left out; each
#                                  with its .folded stacks, for difffolded.pl
#                                  against another round
#   self-rings.txt, self-nfsd.txt  cycles by symbol, self only
#   incl-rings.txt, incl-nfsd.txt  cycles by symbol, with what it calls
#   cost.txt                       WRITEs a second, and each group's busy
#                                  microseconds a WRITE from the run's mpstat
#   top-rings.txt, top-nfsd.txt    the most costly frames, self and with
#                                  what they call, as % of the group's busy
#                                  cycles and as microseconds a WRITE
# The cycle counter does not stop in an idle CPU here (CPU 1 at 95% idle has
# as many samples as CPU 0 at 90% softirq), so idle stacks (do_idle with no
# interrupt on them) are left out and a frame's share of the rest is turned
# into time with mpstat's busy time for the same CPUs over the window.
# Plus the run's own TAG/ and TAG.log, and pool_stats and nfsd's context
# switches around it (TAG-snap0/1.txt).  The probe costs the run some of its
# rate on the rings: take the run's figures from an untraced run beside it.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
FG=${FG:-/opt/FlameGraph}; HZ=999
WHICH=${WHICH:-a}
RINGS=0,2,4,6; [ "$WHICH" = ab ] && RINGS=0,2,4,6,56,58,60,62
NFSD=8-55
busy() {	# FOLDED: the stacks that are not idle, with the weight last
	awk '!(/;do_idle;/ && !/_irq/)' $1
}
top() {	# FOLDED USPERWRITE: frames by self and by inclusive share of busy cycles
	busy $1 | awk -v us=$2 '{
		w = $NF; sub(/ [0-9]+$/, ""); n = split($0, f, ";"); t += w; self[f[n]] += w
		delete seen; for (i = 1; i <= n; i++) if (!(f[i] in seen)) { seen[f[i]]; incl[f[i]] += w } }
	END {
		printf "busy: %.1f us a WRITE\n\nself                                      %%     us/WRITE\n", us
		for (k in self) if (self[k] / t >= 0.004) printf "%-40s %5.2f %7.2f\n", k, 100 * self[k] / t, us * self[k] / t | "sort -k2 -rn"
		close("sort -k2 -rn")
		printf "\nwith what it calls                        %%     us/WRITE\n"
		for (k in incl) if (incl[k] / t >= 0.02) printf "%-40s %5.2f %7.2f\n", k, 100 * incl[k] / t, us * incl[k] / t | "sort -k2 -rn"
	}'
}
report() {	# DIR
	local F=$1 R=${1%-flame} g n c t us
	for g in rings:$RINGS nfsd:$NFSD all:; do
		n=${g%%:*}; c=${g#*:}
		perf script --no-inline -i $F/perf.data ${c:+--cpu $c} 2> /dev/null | $FG/stackcollapse-perf.pl | busy /dev/stdin > $F/$n.folded
		t="$(cat $F/tag 2> /dev/null): $n${c:+ (CPUs $c)}, cycles, idle left out"
		$FG/flamegraph.pl --title "$t" --countname cycles $F/$n.folded > $F/$n.svg
		[ -n "$c" ] || continue
		perf report --no-inline -i $F/perf.data --cpu $c --no-children -g none --percent-limit 0.3 --sort symbol 2> /dev/null | grep -v '^$' > $F/self-$n.txt
		perf report --no-inline -i $F/perf.data --cpu $c --children -g none --percent-limit 2 --sort symbol 2> /dev/null | grep -v '^$' > $F/incl-$n.txt
	done
	read w0 t0 w1 t1 < $F/writes.txt
	ws=$(echo "($w1 - $w0) / ($t1 - $t0)" | bc -l)
	for g in rings:$RINGS nfsd:$NFSD; do
		n=${g%%:*}; c=${g#*:}
		us=$(awk -v c=$c -v ws=$ws 'BEGIN { m = split(c, p, ","); for (i = 1; i <= m; i++) { split(p[i], ab, "-"); for (j = ab[1]; j <= (ab[2] == "" ? ab[1] : ab[2]); j++) want[j] } }
			$1 == "Average:" && ($2 in want) { b += 100 - $NF - $6 } END { printf "%.2f", b / 100 / ws * 1e6 }' $R/srv-mpstat.txt)
		echo "$n (CPUs $c): busy $us us a WRITE"
		top $F/$n.folded $us > $F/top-$n.txt
	done > $F/cost.txt
	sed -i "1i WRITEs a second in the profile: $(printf %.0f $ws)" $F/cost.txt
	cat $F/cost.txt
}
[ "${1:-}" = report ] && { report ${2:?DIR}; exit; }
. ./common.sh
O=${1:?OUTDIR}; tag=${2:?TAG}; WR=${3:-64}
F=$O/$tag-flame; mkdir -p $F; echo $tag > $F/tag
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
snap() { echo "$(date +%s.%N) $(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) $(awk '!/^#/{print $2, $3, $4, $5}' /proc/fs/nfsd/pool_stats) $(for p in $(pgrep -x nfsd); do cat /proc/$p/status; done | awk '/^voluntary_ctxt/{v += $2} /^nonvoluntary_ctxt/{n += $2} END {print v, n}')"; }
writes() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion)"
./settled.sh apply > $F/apply-start.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $F/apply-start.txt | head -3)"
if [ "$WHICH" = ab ]; then for n in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$n; done; else for n in 4 5 6 7; do fstrim /hs/nvme$n; done; fi; say "trimmed, rest 120 s"; sleep 120
client_free || die "client not free"
snap > $O/$tag-snap0.txt
rm -rf $O/$tag
if [ "$WHICH" = ab ]; then
	env WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=$WR ./twoport.sh $O/$tag ab > $O/$tag.log 2>&1 & tp=$!
else
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag WRITES=$WR ./twoport.sh $O/$tag a > $O/$tag.log 2>&1 & tp=$!
fi
while [ ! -e $O/$tag/srv-mpstat.txt ]; do kill -0 $tp 2> /dev/null || break; sleep 0.2; done
if [ -e $O/$tag/srv-mpstat.txt ]; then
	sleep 1; w0=$(writes); t0=$(date +%s.%N)
	perf record -a -g -F 999 -o $F/perf.data -- sleep 8 > $F/perf-record.txt 2>&1
	w1=$(writes); t1=$(date +%s.%N); echo "$w0 $t0 $w1 $t1" > $F/writes.txt
fi
wait $tp; rc=$?
snap > $O/$tag-snap1.txt
[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
[ -s $F/perf.data ] || die "$tag: no profile"
say "$tag | $(grep '^total' $O/$tag.log) | $(grep -E '^loans' $O/$tag.log)"
./settled.sh apply > $F/apply-end.txt 2>&1
say "end: $(tail -1 $F/apply-end.txt)"

report $F
say "DONE: $F"
