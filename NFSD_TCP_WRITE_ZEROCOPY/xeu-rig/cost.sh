#!/bin/bash
# cost.sh RUNDIR...: what a WRITE costs the CPUs in each twoport.sh run
# directory, from srv-mpstat.txt's Average and window.txt's WRITEs a second.
#   ring: port a's ring CPUs (0, 2, 4, 6), %soft
#   nfsd: nfsd's CPUs (8 to 55), %usr + %nice + %sys + %irq + %soft
# It gives the ring and nfsd figures of the FLAMEGRAPH rounds' cost.txt lines.
# Those come from the 100 Hz tick (this kernel has no IRQ time accounting),
# so when the run has perf-stat-window.txt (twoport.sh, from 27 October) it
# also gives the same CPUs' cycles and instructions a WRITE, counted.
for d; do
	w=$(awk '/nfsd in the window/ {for (i=1;i<=NF;i++) if ($i=="WRITEs") {gsub(/,/,"",$(i+1)); print $(i+1)}}' $d/window.txt)
	[ -n "$w" ] || { echo "$d: no WRITEs a second in window.txt" >&2; continue; }
	awk -v w=$w -v t=$(basename $d) '/^Average:/ && $2 ~ /^[0-9]+$/ {c=$2;
		if (c==0||c==2||c==4||c==6) r+=$8;
		if (c>=8 && c<=55) n+=$3+$4+$5+$7+$8}
		END {printf "%s ring %.1f nfsd %.1f us/W (%d WRITEs/s)", t, r*1e4/w, n*1e4/w, w}' $d/srv-mpstat.txt
	# perf stat -A -x,: CPU<n>,count,unit,event,...; the window's length is
	# window.txt's window_s
	[ -s $d/perf-stat-window.txt ] && awk -F, -v w=$w -v s=$(awk '$1 == "a" || $1 == "b" {print $NF; exit}' $d/window.txt) '
		$1 ~ /^CPU[0-9]+$/ && $2 ~ /^[0-9]+$/ {c = substr($1, 4) + 0
			k = (c == 0 || c == 2 || c == 4 || c == 6) ? "r" : (c >= 8 && c <= 55) ? "n" : ""
			if (k != "") v[k, $4] += $2}
		END {if (!s) s = 1
			printf " | ring %.1fk cycles %.1fk instructions, nfsd %.1fk cycles %.1fk instructions a WRITE",
				v["r", "cycles"] / s / w / 1e3, v["r", "instructions"] / s / w / 1e3,
				v["n", "cycles"] / s / w / 1e3, v["n", "instructions"] / s / w / 1e3}' $d/perf-stat-window.txt
	echo
done
