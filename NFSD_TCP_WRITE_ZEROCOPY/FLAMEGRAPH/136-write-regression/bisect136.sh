#!/bin/bash
# bisect136.sh <first run number>: FLAMEGRAPH round 136, the client's
# bisect (Mike, 18 November: "yes, bisect the commits after 1b9af79").
# Started by hand with git bisect start --no-checkout 4c8aa21d29987
# 1b9af79d9f16f -- <the client's paths> (no checkout: nothing written to
# tardis2's root disk but git's bisect state).  Each step: BISECT_HEAD's
# client stack through bstep.sh with one profiled WRITE pass; good at
# 76.5 GB/s or more, bad at 75.8 or less, a second pass between (their
# mean against 76.2); a tree that does not build is skipped.  Ends at
# git bisect's "first bad commit".
cd /home/serviceadmin/snitm/kernel/linux || exit 1
B=NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/136-write-regression
L=/var/tmp/zc-run-logs
n=${1:?first run number}
say() { echo "$(date +%T) $*"; }
gbs() { grep -oE 'total [0-9.]+ GB/s' $L/t2-r$1/write-check.out 2>/dev/null | grep -oE '[0-9.]+' | head -1; }
step=0
while :; do
	c=$(git rev-parse --short=13 BISECT_HEAD) || exit 1
	step=$((step + 1))
	say "step $step: $c $(git log -1 --format='%an: %s' $c)"
	$B/bstep.sh $c B$step $n > $L/541/bisect-step$step.log 2>&1
	g=$(gbs $n)
	if [ -z "$g" ]; then
		say "step $step: $c did not build or run: skip ($(grep -m1 -E 'build failed|exists' $L/541/bisect-step$step.log))"
		[ -e $L/t2-r$n ] && n=$((n + 1))
		out=$(git bisect skip 2>&1)
	else
		v=$(awk -v g=$g 'BEGIN {print (g >= 76.5) ? "good" : (g <= 75.8) ? "bad" : "again"}')
		if [ $v = again ]; then
			n=$((n + 1))
			$B/bstep.sh $c B$step $n >> $L/541/bisect-step$step.log 2>&1
			g2=$(gbs $n)
			v=$(awk -v a=$g -v b=${g2:-0} 'BEGIN {print ((a + b) / 2 >= 76.2) ? "good" : "bad"}')
			g="$g, $g2"
		fi
		say "step $step: $c t2-r$n $g GB/s: $v"
		n=$((n + 1))
		out=$(git bisect $v 2>&1)
	fi
	echo "$out" | head -3
	echo "$out" | grep -q "is the first bad commit" && { git bisect log > $L/541/bisect136.gitlog; say "bisect done"; break; }
	echo "$out" | grep -q "only skipped commits left" && { git bisect log > $L/541/bisect136.gitlog; say "bisect done (skipped commits)"; break; }
done
