#!/bin/bash
# ab-fix.sh "n:txd:f1:f2 ..." - run645.sh read with PROBE=hol, setting
# svc_rtc_conn_full_wait (f1) and svc_rtc_single_sender (f2) per run.
P=/sys/module/sunrpc/parameters
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/104-rx-cpus || exit 1
for r in $1; do
	IFS=: read n txd f1 f2 <<< "$r"
	echo $f1 > $P/svc_rtc_conn_full_wait; echo $f2 > $P/svc_rtc_single_sender
	echo "== t2-r$n txd $txd conn_full_wait $(cat $P/svc_rtc_conn_full_wait) single_sender $(cat $P/svc_rtc_single_sender)"
	PROBE=hol RUNS="$n:read::::::$txd" ./run645.sh 2>&1 | grep -vE "awk: fatal|sends by queue" || exit 1
	echo "$f1 $f2" > /var/tmp/zc-run-logs/t2-r$n/fix.txt
done
echo N > $P/svc_rtc_conn_full_wait; echo N > $P/svc_rtc_single_sender
