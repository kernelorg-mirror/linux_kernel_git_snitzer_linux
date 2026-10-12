#!/bin/bash
# Runs on the client for a cell's length: every RTO, retransmit and
# congestion state change with wall-clock times (perf), and each second
# the TCP counters, every connection to the server and the port's
# counters.  client-rto-watch.sh <dir> [seconds] [port]
O=$1; N=${2:-125}; D=${3:-ens3np0}; mkdir -p $O; cd $O
perf probe -l 2>/dev/null | grep -q tcp_retransmit_timer || perf probe -a 'tcp_retransmit_timer sk=%di' > /dev/null 2>&1
# perf takes no wall clock for these events here: record the offset.
python3 -c 'import time; print(time.time() - time.clock_gettime(time.CLOCK_MONOTONIC))' > mono-offset.txt
perf record -a -k CLOCK_MONOTONIC -e probe:tcp_retransmit_timer -e tcp:tcp_retransmit_skb -e tcp:tcp_cong_state_set -o perf.data -- sleep $N > perf.log 2>&1 &
for i in $(seq $N); do
	t=$(date +%s.%N)
	{ echo "== $t"; cat /proc/net/netstat /proc/net/snmp; } >> netstat.txt
	{ echo "== $t"; ss -tmio '( dport = :2049 )'; } >> ss.txt
	{ echo "== $t"; ethtool -S $D | grep -Ev '^ *(tx|rx|ch)[0-9]+_'; } >> ethtool.txt
	sleep 0.9
done
wait
perf script -i perf.data -F time,cpu,comm,event,trace > events.txt 2> /dev/null
rm -f perf.data
