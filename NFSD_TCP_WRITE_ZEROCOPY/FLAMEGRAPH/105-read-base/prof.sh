#!/bin/bash
# prof.sh <n>: round 105's profile of the READ base (104-rx-cpus/read-prof.sh,
# the strobe with 8 s of every server CPU at 999 Hz) at the base's setting,
# with 5 s of the mesh's memory traffic in the same window, then the host
# settled again without the base's overrides.  The base: one connection to
# a work CPU (svc_rtc_balance Y), RFS (each flow's frames on the CPU that
# last sent on it, its work item's), nconnect 16 a port (32 connections).
# Any of BASE_* overrides it for an A/B.
n=${1:?prof.sh <n>}
H=$(cd "$(dirname "$0")" && pwd); O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
export EXP_RTC_BALANCE=${BASE_BALANCE:-Y} EXP_RFS=${BASE_RFS:-32768} NCONN=${BASE_NCONN:-16} KEEP_FILES=1
(
	for i in $(seq 1200); do [ -e $O/perf.out ] && break; sleep 0.5; done
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mem.txt -- sleep 5
) &
$H/../104-rx-cpus/read-prof.sh $n; rc=$?
wait
(cd $H/../../xeu-rig && env -u EXP_RTC_BALANCE -u EXP_RFS -u NCONN -u EXP_TCP_LOB ./settled.sh apply > $O/apply-settled.txt 2>&1; echo "settled again: $(tail -1 $O/apply-settled.txt)")
gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+')
awk -F, -v gb=$gb '$3 ~ /hnf_mc_reqs/ {printf "memory: %.1f GB/s, %.2f bytes a byte sent\n", $1 * 64 / 5e9, $1 * 64 / 5e9 / gb}' $O/mem.txt
grep placed $O/rtc_stats.w1
exit $rc
