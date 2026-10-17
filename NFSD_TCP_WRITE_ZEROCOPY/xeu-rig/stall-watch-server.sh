#!/bin/bash
# on the server: when a port that was carrying load receives under 2 GB/s for 3 s, dump its counters, connections and nfsd's stacks
o=$1; rm -rf $o; mkdir -p $o
. "$(dirname "$0")/common.sh"	# the host's ports and their addresses
declare -A P LOW HOT DUMPED ADDR
DEVS=""; for p in $PORTS; do ADDR[${PDEVS[$p]}]=${SRVP[$p]}; DEVS="$DEVS ${PDEVS[$p]}"; done
rx() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}'; }
for d in $DEVS; do P[$d]=$(rx $d); LOW[$d]=0; HOT[$d]=0; DUMPED[$d]=0; done
start=$(date +%s); end=$(( start + ${2:-240} ))
while [ $(date +%s) -lt $end ]; do
  for d in $DEVS; do
    c=$(rx $d); r=$(( (c - ${P[$d]}) / 1000000 )); P[$d]=$c
    echo "$(date +%s.%N | cut -c1-14) $d MB/s $r" >> $o/rate.txt
    [ $r -gt 15000 ] && HOT[$d]=$(( ${HOT[$d]} + 1 ))
    if [ ${HOT[$d]} -gt 20 ] && [ $r -ge 0 ] && [ $r -lt 2000 ]; then LOW[$d]=$(( ${LOW[$d]} + 1 )); else LOW[$d]=0; fi
    if [ ${LOW[$d]} -ge 3 ] && [ ${DUMPED[$d]} -lt 3 ] && [ $(( $(date +%s) - start )) -lt 165 ]; then
      DUMPED[$d]=$(( ${DUMPED[$d]} + 1 )); f=$o/dump-$d-${DUMPED[$d]}
      { date +%s.%N; echo "== ethtool"; ethtool -S $d; echo "== ss"; ss -tiemo src ${ADDR[$d]}; echo "== netstat"; cat /proc/net/netstat /proc/net/snmp; echo "== softnet"; cat /proc/net/softnet_stat; echo "== pool_stats"; cat /proc/fs/nfsd/pool_stats; echo "== nfsd stacks"; for t in $(pgrep -x nfsd); do s=$(awk '{print $3}' /proc/$t/stat); [ "$s" = I ] && continue; echo "-- $t $s"; cat /proc/$t/stack 2>/dev/null | head -14; done; } > $f 2>&1
      echo "DUMP $d ${DUMPED[$d]} at $(date +%s)" >> $o/rate.txt; LOW[$d]=0
    fi
  done
  sleep 0.8
done
