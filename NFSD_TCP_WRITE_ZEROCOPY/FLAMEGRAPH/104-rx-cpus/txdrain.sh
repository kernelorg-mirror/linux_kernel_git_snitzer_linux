#!/bin/bash
# txdrain.sh <n> <Y|N>: the READ strobe (client-read.sh <n>, files kept)
# with svc_rtc_balance at <Y|N> and txdrain.bt in its window; beside the
# probe, 25 samples 0.2 s apart of every transmit queue's BQL limit and
# bytes in flight (bql.txt).  Round 104, 17 November.
n=${1:?txdrain.sh <n> <Y|N>} b=${2:?}
H=$(cd "$(dirname "$0")" && pwd); O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O; rm -f $O/txdrain.txt
(
	for i in $(seq 1200); do [ -e $O/txdrain.txt ] && break; sleep 0.5; done
	for k in $(seq 25); do
		for d in enp161s0f0 enp164s0f0; do for q in /sys/class/net/$d/queues/tx-*; do
			echo "$k $d ${q##*/} limit $(cat $q/byte_queue_limits/limit) inflight $(cat $q/byte_queue_limits/inflight)"
		done; done
		sleep 0.2
	done > $O/bql.txt
) &
EXP_RTC_BALANCE=$b KEEP_FILES=1 SRV=txdrain $H/client-read.sh $n; rc=$?
wait
# client-read.sh's own apply after the run kept EXP_RTC_BALANCE: settle again
(cd ../../xeu-rig && ./settled.sh apply > $O/apply-settled.txt 2>&1; echo "settled again: $(tail -1 $O/apply-settled.txt)")
awk '{l[$2" "$3]+=$5; f[$2" "$3]+=$7; c[$2" "$3]++} END {for (q in c) printf "%s: limit %.0f KiB, in flight %.0f KiB (mean of %d)\n", q, l[q]/c[q]/1024, f[q]/c[q]/1024, c[q]}' $O/bql.txt | sort
exit $rc
