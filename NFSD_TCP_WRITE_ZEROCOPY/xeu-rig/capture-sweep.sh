#!/bin/bash
# capture-sweep.sh <outdir> <plan> - sweep.sh, with where.sh and anat.bt run
# inside every cell: where.sh from 35 s in (both ends' CPUs, the rings, a
# profile, the client's view of each connection), then anat.bt for 25 s
# (what a loaned WRITE waits for and how long it holds its credit).  The
# environment is passed on to sweep.sh (STEP_JOBS, STEP_BS, PRP_DIR, ...).
cd "$(dirname "$0")"
O=${1:?outdir}; P=${2:?plan}
./sweep.sh $O $P > $O.log 2>&1 &
SW=$!
seen=" "
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
		./where.sh $O/where-$t-$c 10 > $O/where-$t-$c.txt 2>&1
		timeout -s INT 33 bpftrace anat.bt > $O/anat-$t-$c.txt 2>&1
	done
	sleep 1
done
wait $SW
