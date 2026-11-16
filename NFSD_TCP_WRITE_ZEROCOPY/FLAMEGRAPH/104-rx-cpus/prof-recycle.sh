#!/bin/bash
# prof-recycle.sh <n> <pages>: read-prof.sh <n> (the quick READ strobe with
# 8 s of every server CPU profiled, and rtc_stats either side of the
# window) with sunrpc's svc_tcp_reply_recycle at <pages>, its counters
# saved before and after (recycle.0, recycle.1), then the recycle at 0
# again.  Round 104, 16 November: why the recycle at 512 read 11.5 GB/s
# less than without it (t2-r782 against r781).
P=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle
S=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle_stats
n=${1:?prof-recycle.sh <n> <pages>} pages=${2:?prof-recycle.sh <n> <pages>}
O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
[ -w $P ] || { echo "no svc_tcp_reply_recycle"; exit 1; }
echo $pages > $P || exit 1
cat $S > $O/recycle.0
"$(dirname "$0")/read-prof.sh" $n; rc=$?
cat $S > $O/recycle.1
echo 0 > $P
exit $rc
