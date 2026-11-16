#!/bin/bash
# bqlsample.sh <out> <wire-sec.tsv>: once the load has run 15 s (the 25th row
# of twoport.sh's wire-sec.tsv), five samples a second apart of each port's
# transmit queues' byte queue limits: limit and inflight.  Round 104.
out=$1 W=$2
for i in $(seq 600); do [ -f $W ] && [ $(wc -l < $W) -ge 26 ] && break; sleep 0.5; done
for k in 1 2 3 4 5; do
	for d in enp161s0f0 enp164s0f0; do
		for q in /sys/class/net/$d/queues/tx-*; do
			echo "$k $d ${q##*/} limit $(cat $q/byte_queue_limits/limit) inflight $(cat $q/byte_queue_limits/inflight)"
		done
	done
	sleep 1
done > $out
