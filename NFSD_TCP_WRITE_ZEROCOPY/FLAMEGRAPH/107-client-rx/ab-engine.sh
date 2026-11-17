#!/bin/bash
# ab-engine.sh <n>:<engine>[:fixed] ...: round 107's A/B of fio's engine at
# the client (Mike, 17 November: "please consider using io_uring at the
# fio client"), in turns.  Each run is the READ strobe
# (104-rx-cpus/client-read.sh, files kept) at the settled setting with
# twoport.sh's IOENGINE=<engine> (libaio or io_uring) and FIXEDBUFS=1 with
# "fixed", and from the load's second 6, 5 s of the client's memory
# controllers.  It prints the rate, how busy the client was, its memory
# traffic a byte received, and from client-read.sh's 5 s client profile
# the shares of memcpy, page pinning (try_grab_folio_fast), submission
# (nfs_direct_read_schedule_iovec) and io_uring's own workers (iou-wrk).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
H=$(cd ../FLAMEGRAPH/104-rx-cpus && pwd)
for run in "$@"; do
	IFS=: read n eng fx <<< "$run"
	[ -n "$n" ] && [ -n "$eng" ] || { echo "ab-engine.sh <n>:<engine>[:fixed] ..."; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	(
		W=$O/run/wire-sec.tsv; for i in $(seq 900); do [ -f $W ] && [ $(wc -l < $W) -ge 6 ] && break; sleep 0.2; done
		$SSHC "perf stat -a --per-socket -x, -e uncore_imc/cas_count_read/,uncore_imc/cas_count_write/ -- sleep 5" > $O/cli107-mem.txt 2>&1
		$SSHC "ps -eLo comm | grep -c '^iou-wrk'" > $O/cli107-iouwrk.txt 2>&1
	) &
	FB=; [ "$fx" = fixed ] && FB=1
	KEEP_FILES=1 IOENGINE=$eng FIXEDBUFS=$FB $H/client-read.sh $n > $O/ab-engine.out 2>&1
	wait
	gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+')
	mem=$(awk -F, '/cas_count/ {s+=$3} END {printf "%.1f", s * 1048576 / 5e9}' $O/cli107-mem.txt)
	busy=$(grep -oE 'client CPUs busy: node 0 mean [0-9]+%, node 1 mean [0-9]+%' $O/summary-client.txt | sed 's/client CPUs busy: //')
	sh() { awk -v s="$2" '$NF == s {print $1; exit}' $O/$1; }
	iou=$(awk '$2 ~ /^iou-wrk/ {gsub("%","",$1); t+=$1} END {printf "%.2f%%", t}' $O/cli-perf-comm.txt)
	echo "$(date +%T) t2-r$n $eng${fx:+ $fx}: $gb GB/s | client busy $busy | client memory $mem GB/s, $(awk -v m=$mem -v g=$gb 'BEGIN {printf "%.2f", m / g}') a byte | memcpy $(sh cli-perf-self.txt __pi_memcpy) pinning $(sh cli-perf-self.txt try_grab_folio_fast) submission $(sh cli-perf-children.txt nfs_direct_read_schedule_iovec) iou-wrk $iou (threads $(cat $O/cli107-iouwrk.txt)) | $(grep -oE 'new kernel warnings [0-9]+' $O/ab-engine.out | head -1)"
done
