#!/bin/bash
# ab.sh <n>:<lob>[:<send_max>] ...: round 105's A/B on the READ base, in turns.  Each
# pair is the READ strobe (104-rx-cpus/client-read.sh, files kept) at the
# base (svc_rtc_balance Y, RFS 32768, nconnect 16) with
# net.ipv4.tcp_limit_output_bytes at <lob>, 5 s of the mesh's memory
# traffic in its window, and sunrpc's svc_rtc_send_max at <send_max>
# (0 when not given, held through client-read.sh's settled.sh apply as
# EXP_RTC_SEND_MAX; settled.sh's again after the last); then rate, memory bytes a byte sent, cycles a KiB
# (work CPUs, rings, all), and the server sockets' Send-Q, below-TCP bytes
# and RTT.  Settled without the base after the last.
H=$(cd "$(dirname "$0")" && pwd)
for pair in "$@"; do
	IFS=: read n lob smax <<< "$pair"; smax=${smax:-0}
	P=/sys/module/sunrpc/parameters/svc_rtc_send_max
	if [ -w $P ]; then echo $smax > $P; elif [ $smax != 0 ]; then echo "no svc_rtc_send_max"; exit 1; fi
	O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
	grep -E '^send_full ' /sys/kernel/debug/nfsd/rtc_stats > $O/sf0 2>/dev/null
	(
		for i in $(seq 1200); do [ -s $O/cli-ss.txt ] && break; sleep 0.5; done
		perf stat -a -x, -e arm_cmn/hnf_mc_reqs/ -o $O/mem.txt -- sleep 5
	) &
	EXP_RTC_BALANCE=Y EXP_RFS=32768 NCONN=16 KEEP_FILES=1 EXP_TCP_LOB=$lob EXP_RTC_SEND_MAX=$smax $H/../104-rx-cpus/client-read.sh $n > $O/ab.out 2>&1
	wait
	gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+')
	mem=$(awk -F, '$3 ~ /hnf_mc_reqs/ {printf "%.1f", $1 * 64 / 5e9}' $O/mem.txt)
	cyc=$(awk -F, -v gb=$gb '/^CPU/ && $4=="cycles" {c=substr($1,4)+0; if (c>=8 && c<=55) w+=$2; else g+=$2} END {b=gb*1e9*10; printf "%.0f / %.0f / %.0f", w/b*1024, g/b*1024, (w+g)/b*1024}' $O/run/perf-stat-window.txt)
	sk=$(awk '/skmem/ {match($0,/,t[0-9]+,/); t+=substr($0,RSTART+2,RLENGTH-3); n++} END {printf "%.2f", t/n/1e6}' $O/srv-ss.txt)
	srv=$(grep 'srv sockets' $O/summary-client.txt | grep -oE 'Send-Q mean [0-9.]+ MB|rtt mean [0-9.]+ ms' | tr '\n' ' ')
	sf=$(( $(awk '/^send_full /{print $2}' /sys/kernel/debug/nfsd/rtc_stats 2>/dev/null || echo 0) - $(awk '{print $2}' $O/sf0 2>/dev/null || echo 0) ))
	echo "$(date +%T) t2-r$n lob $lob send_max $smax (receives left for later $sf): $gb GB/s, memory $mem GB/s ($(awk -v m=$mem -v g=$gb 'BEGIN {printf "%.2f", m / g}') a byte), cycles a KiB $cyc, below TCP $sk MB a socket, $srv| $(grep -oE "new kernel warnings [0-9]+" $O/ab.out | head -1)"
done
(cd $H/../../xeu-rig && env -u EXP_RTC_BALANCE -u EXP_RTC_SEND_MAX -u EXP_RFS -u NCONN -u EXP_TCP_LOB ./settled.sh apply > /var/tmp/zc-run-logs/r105/ab-settled.txt 2>&1; echo "settled again: $(tail -1 /var/tmp/zc-run-logs/r105/ab-settled.txt)")
