#!/bin/bash
# memcost-log.sh <prefix> <seconds> <port> [<drive> ...] - beside a run, what
# memcost-read.py needs to say what each line of payload asked of memory:
# every 2 s the mesh's requests to the memory controller and its cache fills
# (two events, which is what the mesh counts at once) with the cores' own
# counters, the port's bytes five times a second, and each named drive's
# sectors written and the region's pages in use.  Start it before the run
# and it stops after <seconds>, or kill it.
#   <prefix>.t0  .cmn.txt  .srv.txt  .drv.txt
. "$(dirname "$0")/common.sh"
P=${1:?prefix}; S=${2:?seconds}; PORT=${3:?port}; shift 3; DRV="${*:-nvme0n1}"
EV=arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/,l3d_cache_lmiss_rd,bus_access_wr
trap 'kill $(jobs -p) 2> /dev/null' EXIT
date +%s.%N > $P.t0
perf stat -a -I 2000 -x, -e $EV -o $P.cmn.txt -- sleep $S 2> /dev/null &
"$RIG/stall-sample.py" $P.srv.txt $S $PORT > /dev/null 2>&1 &
end=$(( $(date +%s) + S ))
while [ $(date +%s) -lt $end ]; do
	echo "$(date +%s.%N) $(for d in $DRV; do awk -v d=$d '$3 == d {print $10}' /proc/diskstats; done | awk '{s += $1} END {print s + 0}') $(( $(reg pages) - $(reg free) ))"
	sleep 2
done > $P.drv.txt
wait
