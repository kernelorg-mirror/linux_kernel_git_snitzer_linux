#!/bin/bash
# The server's side of client-rto-watch.sh: each second the port's
# counters, the TCP counters, every NFS connection, the port's interrupts
# and the CPUs; and every skb dropped with a reason, by the second.
# server-rto-watch.sh <dir> [seconds] [port]
O=$1; N=${2:-125}; D=${3:-enp161s0f0}; mkdir -p $O; cd $O
timeout -s INT $N bpftrace -e 'tracepoint:skb:kfree_skb /args.reason >= 2/ { @[args.reason] = count(); }
	interval:s:1 { printf("== %lld\n", nsecs(sw_tai) / 1000000000 - 37); print(@); clear(@); }' > drops.txt 2>&1 &
for i in $(seq $N); do
	t=$(date +%s.%N)
	{ echo "== $t"; ethtool -S $D; } >> ethtool.txt
	{ echo "== $t"; cat /proc/net/netstat /proc/net/snmp; } >> netstat.txt
	{ echo "== $t"; ss -tmio '( sport = :2049 )'; } >> ss.txt
	{ echo "== $t"; grep -E 'xeu|'$D /proc/interrupts; grep NET_RX /proc/softirqs; } >> irq.txt
	{ echo "== $t"; grep '^cpu' /proc/stat; } >> stat.txt
	sleep 0.85
done
wait
