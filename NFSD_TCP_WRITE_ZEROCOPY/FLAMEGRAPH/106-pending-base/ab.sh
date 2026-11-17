#!/bin/bash
# ab.sh <n>:<send_max>:<nconnect> ...: round 106's A/B on the pending base
# (round 105's: svc_rtc_balance Y, RFS 32768, tcp_limit_output_bytes 256
# KiB), in turns.  Each run is the READ strobe (104-rx-cpus/client-read.sh,
# files kept) with sunrpc's svc_rtc_send_max at <send_max> and nconnect
# <nconnect> a port, held through client-read.sh's settled.sh apply as
# EXP_* (pending-settled.sh's values, with these two overridden), and 5 s
# of the mesh's memory traffic and cache fills in its window.  It prints a
# row of round 106's frame (README.md, "The frame"): GB/s, DMA, memory,
# memory a byte sent, cache fills, below TCP and Send-Q a socket, the
# replies held at the server in all, cycles a KiB (work CPUs, rings, all),
# the receives held back and the write space refused.  Settled after the
# last.
H=$(cd "$(dirname "$0")" && pwd); R=$H/../../xeu-rig
st() { awk -v k="$1" '$1 == k {print $2}' /sys/kernel/debug/nfsd/rtc_stats 2>/dev/null || echo 0; }
for run in "$@"; do
	IFS=: read n smax nconn <<< "$run"
	[ -n "$n" ] && [ -n "$smax" ] && [ -n "$nconn" ] || { echo "ab.sh <n>:<send_max>:<nconnect> ..."; exit 1; }
	O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
	sf0=$(st send_full); ns0=$(st send_nospace)
	(
		for i in $(seq 1200); do [ -s $O/cli-ss.txt ] && break; sleep 0.5; done
		perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mem.txt -- sleep 5
	) &
	EXP_RTC_BALANCE=Y EXP_RFS=32768 EXP_TCP_LOB=262144 EXP_RTC_SEND_MAX=$smax NCONN=$nconn KEEP_FILES=1 \
		$H/../104-rx-cpus/client-read.sh $n > $O/ab.out 2>&1
	wait
	gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+')
	mem=$(awk -F, '$3 ~ /hnf_mc_reqs/ {printf "%.1f", $1 * 64 / 5e9}' $O/mem.txt)
	fill=$(awk -F, '$3 ~ /hnf_cache_fill/ {printf "%.1f", $1 * 64 / 5e9}' $O/mem.txt)
	cyc=$(awk -F, -v gb=$gb '/^CPU/ && $4=="cycles" {c=substr($1,4)+0; if (c>=8 && c<=55) w+=$2; else g+=$2} END {b=gb*1e9*10; printf "%.0f / %.0f / %.0f", w/b*1024, g/b*1024, (w+g)/b*1024}' $O/run/perf-stat-window.txt)
	below=$(awk '/skmem/ {match($0,/,t[0-9]+,/); t+=substr($0,RSTART+2,RLENGTH-3); n++} END {printf "%.2f", t/n/1e6}' $O/srv-ss.txt)
	srvl=$(grep 'srv sockets' $O/summary-client.txt)
	socks=$(grep -oE 'srv sockets \([0-9]+\)' <<< "$srvl" | grep -oE '[0-9]+')
	sendq=$(grep -oE 'Send-Q mean [0-9.]+' <<< "$srvl" | grep -oE '[0-9.]+$')
	held=$(awk -v s=$socks -v q=$sendq -v b=$below 'BEGIN {printf "%.0f", s * (q + b)}')
	echo "$(date +%T) t2-r$n send_max $smax nconnect $nconn: $gb GB/s | DMA $(awk -v g=$gb 'BEGIN {printf "%.1f", 2 * g}') | memory $mem GB/s, $(awk -v m=$mem -v g=$gb 'BEGIN {printf "%.2f", m / g}') a byte | cache fills $fill | below TCP $below MB, Send-Q $sendq MB a socket, $socks sockets, held $held MB | cycles a KiB $cyc | held back $(( $(st send_full) - sf0 )), write space refused $(( $(st send_nospace) - ns0 )) | $(grep -oE 'new kernel warnings [0-9]+' $O/ab.out | head -1)"
done
(cd $R && env -u EXP_RTC_BALANCE -u EXP_RTC_SEND_MAX -u EXP_RFS -u NCONN -u EXP_TCP_LOB ./settled.sh apply > /var/tmp/zc-run-logs/r106/ab-settled.txt 2>&1; echo "settled again: $(tail -1 /var/tmp/zc-run-logs/r106/ab-settled.txt)")
