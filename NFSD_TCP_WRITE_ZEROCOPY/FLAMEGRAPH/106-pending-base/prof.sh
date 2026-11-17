#!/bin/bash
# prof.sh <n> settled|pending: round 106's profile of the READ strobe
# (104-rx-cpus/read-prof.sh: 8 s of every server CPU at 999 Hz with call
# chains, the client's side beside it) at the settled setting or at
# pending-settled.sh's (round 105's base and the send bound), with 5 s of
# the mesh's memory traffic in the same window, the files kept
# (KEEP_FILES=1), and the host settled after.
n=${1:?prof.sh <n> settled|pending}; how=${2:?prof.sh <n> settled|pending}
H=$(cd "$(dirname "$0")" && pwd); R=$H/../../xeu-rig
O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
export KEEP_FILES=1
(
	for i in $(seq 1200); do [ -e $O/perf.out ] && break; sleep 0.5; done
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mem.txt -- sleep 5
) &
case $how in
settled) $H/../104-rx-cpus/read-prof.sh $n; rc=$?
	(cd $R && ./settled.sh apply > $O/apply-settled.txt 2>&1; echo "settled again: $(tail -1 $O/apply-settled.txt)") ;;
pending) $R/pending-settled.sh run $H/../104-rx-cpus/read-prof.sh $n; rc=$? ;;
*) echo "settled or pending"; exit 1 ;;
esac
wait
gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+')
echo "t2-r$n ($how): $(grep -m1 '^total' $O/run.log)"
awk -F, -v gb=$gb '$3 ~ /hnf_mc_reqs/ {printf "memory: %.1f GB/s, %.2f bytes a byte sent\n", $1 * 64 / 5e9, $1 * 64 / 5e9 / gb}' $O/mem.txt
grep placed $O/rtc_stats.w1 2>/dev/null
exit $rc
