#!/bin/bash
# rwbase.sh <build> [rounds]: READs and a 70/30 mix of READs and WRITEs from
# the client, with a write run beside them, to lay one kernel beside another
# (the 7.1.13 baseline is build 464's, 14 October: results-rwbase-464.tsv and
# "Reads and mixes" in README.md).  Each load through twoport.sh, so the
# rules of a run hold: rest first, every drive inside its fast region, the
# drives write through.  Two ports at the two-port setting, four drives a
# port; then one port over all eight drives at the one-port setting.  Each
# run is $L/rwbase-<tag>-<round>; a line of each goes to $L/rwbase-<build>.tsv.
# About 25 min a round.
cd "$(dirname "$0")" || exit 1
. ./common.sh
B=${1:?build}; N=${2:-2}; T=$L/rwbase-$B.tsv
two() {	# two <tag> <env...>
	local tag=$1; shift
	env RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 RWTSV=$T TAG=$tag "$@" ./twoport.sh $L/rwbase-$tag > $L/rwbase-$tag.log 2>&1
	echo "$(date +%T) $tag rc=$? $(grep '^total' $L/rwbase-$tag.log)"
}
one() {
	local tag=$1; shift
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3 /hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" FS_B= RUNTIME=60 LOWAT=0 RWTSV=$T TAG=$tag "$@" ./twoport.sh $L/rwbase-$tag a > $L/rwbase-$tag.log 2>&1
	echo "$(date +%T) $tag rc=$? $(grep '^total' $L/rwbase-$tag.log)"
}
for r in $(seq 1 $N); do
	two ab-read-8m-$r      RW=read
	two ab-randread-1m-$r  RW=randread BS=1M
	two ab-randread-4k-$r  RW=randread BS=4k
	two ab-rw70-8m-$r      RW=rw RWMIX=70
	two ab-randrw70-1m-$r  RW=randrw RWMIX=70 BS=1M
	two ab-randrw70-4k-$r  RW=randrw RWMIX=70 BS=4k
	two ab-write-8m-$r     RW=write
	one a8-read-8m-$r      RW=read
	one a8-rw70-8m-$r      RW=rw RWMIX=70
done
