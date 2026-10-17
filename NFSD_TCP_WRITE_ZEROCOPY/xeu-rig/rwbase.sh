#!/bin/bash
# rwbase.sh <build> [rounds] [first round]: READs and a 70/30 mix of READs and WRITEs from
# the client, with a write run beside them, to lay one kernel beside another
# (the 7.1.13 baseline is build 464's, 14 October: results-rwbase-464.tsv and
# "Reads and mixes" in README.md).  Each load through twoport.sh, so the
# rules of a run hold: rest first, every drive inside its fast region, the
# drives write through.  Two ports at the two-port setting, four drives a
# port; then one port over all eight drives at the one-port setting.  Each
# run is $L/rwbase-<build>-<tag>-<round>, so that one kernel's runs never
# replace another's; a line of each goes to $L/rwbase-<build>.tsv.  Rounds
# are numbered from [first round] (1), so that more rounds can be added.
# About 30 min a round.  It stops at the first run that fails, a refusal by
# settled.sh's audit included.
cd "$(dirname "$0")" || exit 1
. ./common.sh
B=${1:?build}; N=${2:-2}; R0=${3:-1}; T=$L/rwbase-$B.tsv
two() {	# two <tag> <env...>
	local tag=$1; shift
	which_ok ab || exit 1	# not on a host with one port (tardis2)
	env RCVBUF=393216 ENTRIES=128 LINGER=0 LOWAT=0 RWTSV=$T TAG=$tag "$@" ./twoport.sh $L/rwbase-$B-$tag > $L/rwbase-$B-$tag.log 2>&1
	local rc=$?
	echo "$(date +%T) $tag rc=$rc $(grep '^total' $L/rwbase-$B-$tag.log)"
	[ $rc = 0 ] || { echo "stopped: $(grep -E 'BAD|FAILED' $L/rwbase-$B-$tag.log | head -5)"; exit 1; }
}
one() {
	local tag=$1; shift
	env FS_A="$ALL_FS" FS_B= RUNTIME=60 LOWAT=0 RWTSV=$T TAG=$tag "$@" ./twoport.sh $L/rwbase-$B-$tag a > $L/rwbase-$B-$tag.log 2>&1
	local rc=$?
	echo "$(date +%T) $tag rc=$rc $(grep '^total' $L/rwbase-$B-$tag.log)"
	[ $rc = 0 ] || { echo "stopped: $(grep -E 'BAD|FAILED' $L/rwbase-$B-$tag.log | head -5)"; exit 1; }
}
for r in $(seq $R0 $((R0 + N - 1))); do
	two ab-read-8m-$r      RW=read
	two ab-randread-1m-$r  RW=randread BS=1M
	two ab-randread-4k-$r  RW=randread BS=4k
	two ab-rw70-8m-$r      RW=rw RWMIX=70
	two ab-randrw70-1m-$r  RW=randrw RWMIX=70 BS=1M
	two ab-randrw70-4k-$r  RW=randrw RWMIX=70 BS=4k
	two ab-write-8m-$r     RW=write
	two ab-write-1m-$r     RW=write BS=1M
	two ab-randwrite-1m-$r RW=randwrite BS=1M
	one a8-read-8m-$r      RW=read
	one a8-rw70-8m-$r      RW=rw RWMIX=70
done
