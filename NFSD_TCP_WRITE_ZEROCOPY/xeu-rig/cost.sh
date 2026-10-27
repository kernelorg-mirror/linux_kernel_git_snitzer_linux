#!/bin/bash
# cost.sh RUNDIR...: what a WRITE costs the CPUs in each twoport.sh run
# directory, from srv-mpstat.txt's Average and window.txt's WRITEs a second.
#   ring: port a's ring CPUs (0, 2, 4, 6), %soft
#   nfsd: nfsd's CPUs (8 to 55), %usr + %nice + %sys + %irq + %soft
# It gives the ring and nfsd figures of the FLAMEGRAPH rounds' cost.txt lines.
for d; do
	w=$(awk '/nfsd in the window/ {for (i=1;i<=NF;i++) if ($i=="WRITEs") {gsub(/,/,"",$(i+1)); print $(i+1)}}' $d/window.txt)
	[ -n "$w" ] || { echo "$d: no WRITEs a second in window.txt" >&2; continue; }
	awk -v w=$w -v t=$(basename $d) '/^Average:/ && $2 ~ /^[0-9]+$/ {c=$2;
		if (c==0||c==2||c==4||c==6) r+=$8;
		if (c>=8 && c<=55) n+=$3+$4+$5+$7+$8}
		END {printf "%s ring %.1f nfsd %.1f us/W (%d WRITEs/s)\n", t, r*1e4/w, n*1e4/w, w}' $d/srv-mpstat.txt
done
