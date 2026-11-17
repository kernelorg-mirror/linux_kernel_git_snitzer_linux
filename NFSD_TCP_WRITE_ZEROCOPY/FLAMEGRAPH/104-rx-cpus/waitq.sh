#!/bin/bash
# waitq.sh <n>: where a READ reply's bytes wait (round 104, 17 November,
# Mike: "move on to finding where a reply's bytes wait").  The READ strobe
# at the settled setting (client-read.sh <n>, files kept) with
# skbshape.bt, and in its window 10 samples, 0.5 s apart, each taken at
# one instant of: every NFS socket's Send-Q, unacked segments, mss and
# skmem t (ss -tmi), every transmit queue's qdisc backlog (tc -s) and
# BQL bytes in flight, and the client's NFS sockets' Recv-Q.  EXP_RTC_*
# pass through to settled.sh for the run and are dropped after it.  waitq.py
# sums each stage over the sockets and divides it by the rate.
n=${1:?waitq.sh <n>}
H=$(cd "$(dirname "$0")" && pwd); O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O; rm -f $O/skbshape.txt $O/waitq.txt
. $H/../../xeu-rig/common.sh
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -i $KEY root@$CLIMGMT"
(
	for i in $(seq 1200); do [ -e $O/skbshape.txt ] && break; sleep 0.5; done
	for k in $(seq 10); do
		{
			echo "# sample $k $(date +%s.%N)"
			ss -tmiH state established '( sport = :2049 )' | sed 's/^/ss /'
			for d in enp161s0f0 enp164s0f0; do
				tc -s qdisc show dev $d | awk -v d=$d '/^qdisc fq_codel/ {q=$5} /backlog/ && q {print "qdisc", d, q, $2; q=""}'
				for q in /sys/class/net/$d/queues/tx-*; do echo "bql $d ${q##*/} $(cat $q/byte_queue_limits/inflight)"; done
			done
			$SSHC "ss -tnH state established '( dport = :2049 )'" < /dev/null | awk '{print "cli", $1, $2}'
		} >> $O/waitq.txt
		sleep 0.5
	done
) &
KEEP_FILES=1 SRV=skbshape $H/client-read.sh $n; rc=$?
wait
# the run's own settle after keeps any EXP_ override: settle without them
(cd $H/../../xeu-rig && env -u EXP_RTC_CPUS -u EXP_RTC_BALANCE ./settled.sh apply > $O/apply-settled.txt 2>&1; echo "settled again: $(tail -1 $O/apply-settled.txt)")
python3 -I $H/waitq.py $O
exit $rc
