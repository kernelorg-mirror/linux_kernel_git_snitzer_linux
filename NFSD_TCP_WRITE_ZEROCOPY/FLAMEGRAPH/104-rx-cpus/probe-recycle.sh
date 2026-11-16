#!/bin/bash
# probe-recycle.sh <n> <pages> <probe>: client-read.sh <n> (the quick READ
# strobe) with $(dirname $0)/<probe>.bt in its window and sunrpc's
# svc_tcp_reply_recycle at <pages>, its counters saved before and after
# (recycle.0, recycle.1), then the recycle at 0 again.  Round 104.
P=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle
S=/sys/module/sunrpc/parameters/svc_tcp_reply_recycle_stats
n=${1:?probe-recycle.sh <n> <pages> <probe>} pages=${2:?} probe=${3:?}
O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
[ -w $P ] || { echo "no svc_tcp_reply_recycle"; exit 1; }
echo $pages > $P || exit 1
cat $S > $O/recycle.0
SRV=$probe "$(dirname "$0")/client-read.sh" $n; rc=$?
cat $S > $O/recycle.1
echo 0 > $P
exit $rc
