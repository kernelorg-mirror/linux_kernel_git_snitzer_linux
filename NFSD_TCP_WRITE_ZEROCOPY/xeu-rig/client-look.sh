#!/bin/bash
# Runs on the client during a cell: 20 s of counters either side, then a
# 10 s system-wide profile.  Writes to the directory given.
O=$1; D=ens3np0; mkdir -p $O; cd $O
snap() { cat /proc/self/mountstats > mountstats.$1; cat /proc/net/netstat /proc/net/snmp > netstat.$1
	ethtool -S $D > ethtool.$1; tc -s qdisc show dev $D > qdisc.$1; cat /proc/interrupts > interrupts.$1
	cat /proc/net/softnet_stat > softnet.$1; ss -tmi '( dport = :2049 )' > ss.$1; cat /proc/stat > stat.$1; }
snap 0
top -H -b -n 2 -d 10 -w 200 | awk '/^top/{n++} n==2' | head -80 > top.txt
ps -eLo pid,tid,psr,pcpu,comm | grep -E 'fio|rpciod|xprtiod|nfsiod|kworker' | sort -k4 -nr | head -80 > threads.txt
sleep 10
snap 1
perf record -a -g -F 499 -o perf.data -- sleep 10 > perf.log 2>&1
perf report -i perf.data --no-children --no-inline -g none --percent-limit 0.4 --sort symbol 2>/dev/null | grep -v '^$' | head -90 > perf-sym.txt
perf report -i perf.data --no-children --no-inline -g none --percent-limit 0.5 --sort comm 2>/dev/null | grep -v '^$' | head -40 > perf-comm.txt
perf report -i perf.data --no-children --no-inline -g caller,0.5,callee --percent-limit 2 --sort symbol 2>/dev/null | head -400 > perf-cg.txt
