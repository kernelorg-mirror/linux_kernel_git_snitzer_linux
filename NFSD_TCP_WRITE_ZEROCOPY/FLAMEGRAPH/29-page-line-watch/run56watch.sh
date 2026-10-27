#!/bin/bash
# run56watch.sh: FLAMEGRAPH round 29, who writes a region page's struct page
# line.  On the tip's sunrpc (a-sunrpc.ko of t2-r50, round 26's,
# A06FE478658242A10E100FF), the settled setting (xeu rx_prefetch Y): one
# loan64 run, and from 5 s after the WRITEs pass 10,000 a second, four
# region pages in turn, each picked then with both halves posted in a ring
# (pickpage.py), watched for 3 s with hardware watchpoints on its line:
# writes to flags (+0), pp_ref_count (+40) and _mapcount/_refcount (+48), and
# reads and writes of pp_magic (+8), the word napi_build_skb() and
# svc_tcp_rx_borrow_segment() read.  Leaves build 499's own sunrpc loaded.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r53
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
A=A06FE478658242A10E100FF N=3F0C341E86E266D5FDF2ADF K=EFD53C53F61C1F0CDF37D89
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
load() {	# load A|K TAG
	local want
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	case $1 in
	K) rm -f /lib/modules/$(uname -r)/updates/sunrpc.ko; want=$K
	   SUNRPC_KO=none ./swap-sunrpc.sh > $O/swap-$2.txt 2>&1 || die "$2: the reload: $(tail -2 $O/swap-$2.txt)" ;;
	A) want=$A
	   SUNRPC_KO=$L/t2-r50/a-sunrpc.ko ./swap-sunrpc.sh > $O/swap-$2.txt 2>&1 || die "$2: the reload: $(tail -2 $O/swap-$2.txt)" ;;
	esac
	[ "$(cat /sys/module/sunrpc/srcversion)" = $want ] || die "$2: sunrpc $(cat /sys/module/sunrpc/srcversion), not $want"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$2: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$2: nfs-server did not start"
	./settled.sh apply > $O/apply-$2.txt 2>&1 || die "$2 not settled: $(grep -E 'BAD|FAILED' $O/apply-$2.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$2: a new WARNING in dmesg"
}
watch() {	# watch N: pick a page and watch its line for 3 s
	local p ev o a m
	p=$(python3 -I $O/pickpage.py 1); [ -n "$p" ] || { echo "w$1: no page" >> $O/watch.when; return; }
	ev=; for o in 0 8 40 48; do a=$(printf '%x' $((0x$p + o))); m=w; [ $o = 8 ] && m=rw; ev="$ev -e mem:0x$a/8:$m"; done
	echo "w$1: page $p from $(date +%T.%N)" >> $O/watch.when
	perf record -a -g $ev -o $O/w$1.data -- sleep 3 > $O/w$1-record.txt 2>&1
	perf script -i $O/w$1.data -F comm,tid,cpu,time,event,ip,sym > $O/w$1.txt 2> /dev/null
}

W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), sunrpc $(cat /sys/module/sunrpc/srcversion), nfsd $(cat /sys/module/nfsd/srcversion), xeu $(cat /sys/module/xeu/srcversion) rx_prefetch $(cat /sys/module/xeu/parameters/rx_prefetch)"
load A watch
say "sunrpc $(cat /sys/module/sunrpc/srcversion) | $(tail -1 $O/apply-watch.txt)"
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
client_free || die "client not free"
( w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
  for t in $(seq 120); do a=$(w); sleep 1; b=$(w); [ $((b - a)) -gt 10000 ] && break; done
  sleep 5; echo "watch from $(date +%T), $((b - a)) WRITEs a second" > $O/watch.when
  for n in 1 2 3 4; do watch $n; done ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=w56-loan64 WRITES=64 ./twoport.sh $O/w56-loan64 a > $O/w56-loan64.log 2>&1; rc=$?
wait
[ $rc = 0 ] || die "w56: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/w56-loan64.log | head -3)"
say "w56-loan64 | $(grep '^total' $O/w56-loan64.log) | $(grep -E '^loans' $O/w56-loan64.log) | $(head -1 $O/watch.when)"
for n in 1 2 3 4; do say "w$n: $(grep "^w$n:" $O/watch.when | cut -d' ' -f2-3) $(wc -l < $O/w$n.txt 2>/dev/null) hits"; done
load K end
say "end: sunrpc $(cat /sys/module/sunrpc/srcversion); $(tail -1 $O/apply-end.txt); DONE"
