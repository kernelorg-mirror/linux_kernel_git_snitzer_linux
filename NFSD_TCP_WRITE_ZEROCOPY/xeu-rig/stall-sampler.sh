#!/bin/bash
# sampler.sh <outfile> <seconds>: once a second, both ports' ring bytes, each data drive's sectors written
# and busy ms, nfsd threads by state, and the server's TCP counters
o=$1; n=$2
for s in $(seq 1 $n); do
  a=$(ethtool -S enp161s0f0 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}')
  b=$(ethtool -S enp164s0f0 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}')
  d=$(awk '$3 ~ /^nvme[0-7]n1$/{printf "%s:%s:%s ", $3, $10, $13}' /proc/diskstats)
  t=$(ps -eo stat,comm | awk '$2=="nfsd"{c[substr($1,1,1)]++} END{printf "R=%d D=%d I=%d S=%d", c["R"], c["D"], c["I"], c["S"]}')
  x=$(awk '/^TcpExt:/{if(!h){split($0,k);h=1}else{for(i=2;i<=NF;i++) if(k[i]~/^(TCPRcvQDrop|TCPBacklogDrop|TCPZeroWindowDrop|TCPOFOQueue|ListenDrops|TCPWantZeroWindowAdv|TCPToZeroWindowAdv|TCPFromZeroWindowAdv)$/) printf "%s=%s ", k[i], $i}}' /proc/net/netstat)
  echo "$(date +%s.%N | cut -c1-14) rx $a $b | $d| $t | $x"
  sleep 0.85
done > $o
