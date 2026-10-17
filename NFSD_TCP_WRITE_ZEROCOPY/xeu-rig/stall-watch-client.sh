#!/bin/bash
# on the client: once a second each port's bytes on the wire and its qdisc backlog; when a port that was
# carrying load sends under 2 Gbit/s... 0.25 GB/s for 3 s, dump its queues, counters and connections, twice
# $2: the server's host part on each port, 211 (tardis1, the default) or 213 (tardis2)
S=${2:-211}
o=/tmp/cl-watch; rm -rf $o; mkdir -p $o
declare -A P L LOW HOT DUMPED
for d in ens3np0 ens6np0; do P[$d]=$(ethtool -S $d | awk '/tx_bytes_phy:/{print $2}'); LOW[$d]=0; HOT[$d]=0; DUMPED[$d]=0; done
dump() { d=$1; n=$2; f=$o/dump-$d-$n
  { date +%s.%N; echo "== tc"; tc -s -d qdisc show dev $d; echo "== ethtool"; ethtool -S $d; echo "== ss"; ss -tiemo dst $3; echo "== softirqs"; cat /proc/softirqs; echo "== interrupts"; grep -E "CPU0|mlx5" /proc/interrupts; echo "== netstat"; cat /proc/net/netstat /proc/net/snmp; echo "== softnet"; cat /proc/net/softnet_stat; echo "== rpc"; cat /sys/kernel/debug/sunrpc/rpc_xprt/*/info 2>/dev/null | head -400; } > $f 2>&1; }
start=$(date +%s); end=$(( start + ${1:-240} ))
while [ $(date +%s) -lt $end ]; do
  for d in ens3np0 ens6np0; do
    c=$(ethtool -S $d | awk '/tx_bytes_phy:/{print $2}'); r=$(( (c - ${P[$d]}) / 1000000 )); P[$d]=$c
    b=$(tc -s qdisc show dev $d | awk '/backlog/{gsub("b","",$2); s+=$2; p+=$3} END{print s+0}')
    echo "$(date +%s.%N | cut -c1-14) $d MB/s $r backlog $b" >> $o/rate.txt
    [ $r -gt 15000 ] && HOT[$d]=$(( ${HOT[$d]} + 1 ))
    if [ ${HOT[$d]} -gt 20 ] && [ $r -lt 2000 ]; then LOW[$d]=$(( ${LOW[$d]} + 1 )); else LOW[$d]=0; fi
    if [ ${LOW[$d]} -ge 3 ] && [ ${DUMPED[$d]} -lt 3 ] && [ $(( $(date +%s) - start )) -lt 165 ]; then
      case $d in ens3np0) s=192.168.1.$S;; *) s=192.168.0.$S;; esac
      DUMPED[$d]=$(( ${DUMPED[$d]} + 1 )); dump $d ${DUMPED[$d]} $s; echo "DUMP $d ${DUMPED[$d]} at $(date +%s)" >> $o/rate.txt; LOW[$d]=0
    fi
  done
  sleep 0.8
done
