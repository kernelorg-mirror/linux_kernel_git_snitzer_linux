#!/bin/bash
# run177.sh: round 60, where the lookahead's stalls went.  One loan run on
# both ports at the two-port setting with the sunrpc of the A/B in
# updates/; from 5 s into the timed window, on nfsd's CPUs (8 to 55), for
# svc_tcp_rx_lookahead 0, 2, 0 in turn: 4 s of samples of backend memory
# stall cycles, then 4 s of cycles.  Each window's WRITEs are counted from
# perf's own start to its end.  Ends with the switch at 2.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r177
mkdir -p $O
SP=/sys/module/sunrpc/parameters/svc_tcp_rx_lookahead
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; echo 2 > $SP; exit 1; }
K=796267B1BA39136191F34A9
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || die "kdump not armed"
[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "sunrpc is not $K"
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
echo 0 > $SP
sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
t=r177-ab
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  timed_load $O/$t 5 || exit 1
  for v in 0 2 0; do
	echo $v > $SP
	sleep 1
	for e in stall_backend_mem:100000 cycles:200000; do
		IFS=: read ev c <<< "$e"
		n=$ev-$v-$(date +%s)
		perf record -C 8-55 -e $ev -c $c -o $O/$n.data -- sh -c "awk '/^proc3/{print \$10}' /proc/net/rpc/nfsd > $O/$n.w0; sleep 4; awk '/^proc3/{print \$10}' /proc/net/rpc/nfsd > $O/$n.w1" > /dev/null 2>&1
		echo "$n $(( $(cat $O/$n.w1) - $(cat $O/$n.w0) ))" >> $O/record-writes.txt
	done
  done
  echo 2 > $SP ) &
env WARM=10 RUNTIME=45 TSV=$O/results.tsv TAG=$t WRITES=128 ./twoport.sh $O/$t ab > $O/$t.log 2>&1; rc=$?
wait
echo 2 > $SP
[ $rc = 0 ] || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
say "$t | $(grep '^total' $O/$t.log) | $(grep -c 'no drive stepped down' $O/$t.log) no-step | $(./cost.sh $O/$t)"
while read n w; do
	perf report -i $O/$n.data --stdio --sort sym -F overhead,sample,sym 2> /dev/null | grep -v "^#" | grep . | sort -k2 -rn | head -60 > $O/samples-$n.txt
	echo "$n WRITEs $w" >> $O/samples-$n.txt
done < $O/record-writes.txt
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $SP $(cat $SP); $(tail -1 $O/apply-end.txt); DONE"
