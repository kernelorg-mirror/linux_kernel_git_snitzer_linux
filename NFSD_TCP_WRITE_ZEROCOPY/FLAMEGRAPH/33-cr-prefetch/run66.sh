#!/bin/bash
# run66.sh: round 33's look before any change.  Round 28's prefetch already
# reads the next completion entry (one 64-byte line) a frame early; a
# prefetch further ahead helps only if the card has written those entries
# by then.  So: how many frames does each poll of port a's rings take, and
# how often does a poll end because the ring is empty rather than at its
# budget?  One loan64 run at the current state, napi:napi_poll counted
# from 5 s after the WRITEs pass 10,000 a second, for 10 s.  xeu polls its
# transmit rings by NAPI too: receive polls run on CPUs 0, 2, 4, 6 and
# transmit polls on 1, 3, 5, 7, so the counts are kept apart by CPU.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r66${RUN:+-$RUN}
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
client_free || die "client not free"
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
./settled.sh apply > $O/apply.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
t=r66-look-loan64
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  sleep 20
  for s in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "look from $(date +%T), $((b - a)) WRITEs a second" > $O/look.when
  w0=$(w)
  timeout -s INT 10 bpftrace -e 'tracepoint:napi:napi_poll /str(args.dev_name) == "enp161s0f0"/ {
	$k = cpu % 2 ? "tx" : "rx";
	@work[$k] = lhist(args.work, 0, 65, 4); @polls[$k] = count(); @frames[$k] = sum(args.work);
	if (args.work >= args.budget) { @at_budget[$k] = count(); } }' > $O/napi-poll.txt 2>&1 < /dev/null
  w1=$(w); echo "WRITEs in the look: $((w1 - w0))" >> $O/napi-poll.txt ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1 || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
wait
say "$t | $(grep '^total' $O/$t.log) | $(./cost.sh $O/$t) | $(cat $O/look.when)"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "end not settled"
say "end: $(tail -1 $O/apply-end.txt); DONE"
