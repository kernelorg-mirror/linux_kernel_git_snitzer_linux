#!/bin/bash
# iperf-tx.sh <a|b|ab> [streams per port] [seconds] - what the wire
# carries OUT of one port, the other, or both at once with no NFS and no
# drives (16 November, round 104: the READ direction): <streams> iperf3
# processes on each port, the senders on nfsd's CPUs (8-55), the client
# receiving (iperf3 -R), sendfile zero-copy (-Z) as nfsd sends page
# references, LEN (256K) a write.  The rate is read from the port's
# tx_bytes over the middle of the run.  iperf2port.sh is the other way.
# Ports as twoport.sh has them.
. "$(dirname "$0")/common.sh"
WHICH=${1:?a|b|ab}; N=${2:-16}; SECS=${3:-20}
which_ok "$WHICH" || exit 1
declare -A DEV=([a]=${PDEVS[a]} [b]=${PDEVS[b]}) SRV=([a]=${SRVP[a]} [b]=${SRVP[b]}) CLI=([a]=${CLIP[a]} [b]=${CLIP[b]})
PORTS=$(sed 's/./& /g' <<< "$WHICH")
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -i $KEY root@$CLIMGMT"
fifo() { ethtool -S $1 | awk '/port_rx_fifo_tc[0-9]_drop_packets/{s+=$2} END{print s+0}'; }
ringdrop() { ethtool -S $1 | awk '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
rxbytes() { cat /sys/class/net/$1/statistics/tx_bytes; }
shares() { for q in /sys/class/net/$1/queues/tx-*; do cat $q/byte_queue_limits/inflight 2>/dev/null || echo 0; done | tr '\n' ' '; }
client_free || exit 1
trap 'pkill -x iperf3; pkill -f "[w]ire-rcv[.]py"; $SSHC "pkill -x iperf3; pkill -f "[w]ire-snd[.]py"" < /dev/null; echo N > /sys/module/xeu/parameters/rx_port_stats' EXIT
echo Y > /sys/module/xeu/parameters/rx_port_stats
# Each port's ring interrupts on its own CPUs, as twoport.sh has them.
# The host at the settled setting (settled.sh), its CPU layout put in place
# again first for the ports used.
"$RIG/settled.sh" place $PORTS || exit 1
audit_or_die rest $L/iperf-tx-audit.txt
pkill -x iperf3; sleep 1
c=""
	for p in $PORTS; do for i in $(seq $N); do
		taskset -c 8-55 iperf3 -s -1 -B ${SRV[$p]} -p $(( 5200 + i )) > /dev/null 2>&1 &
	done; done
	for p in $PORTS; do c="$c for i in \$(seq $N); do iperf3 -c ${SRV[$p]} -B ${CLI[$p]} -p \$(( 5200 + i )) -t $SECS -R -Z -l ${LEN:-256K} > /dev/null 2>&1 & done;"; done
sleep 2
$SSHC "$c wait" < /dev/null &
W=$(( SECS - 8 )); sleep 5
declare -A F0 T0 B0 S0
for p in $PORTS; do d=${DEV[$p]}; F0[$p]=$(fifo $d); T0[$p]=$(ringdrop $d); B0[$p]=$(rxbytes $d); S0[$p]=$(shares $d); done
echo 1 > $RG
mpstat -P ALL $W 1 > /tmp/.iperf-tx-mpstat 2>&1 &
sleep $W
tot=0
for p in $PORTS; do
	d=${DEV[$p]}; f1=$(fifo $d); t1=$(ringdrop $d); b1=$(rxbytes $d); s1=$(shares $d)
	sh=$(paste <(tr ' ' '\n' <<< "${S0[$p]}") <(tr ' ' '\n' <<< "$s1") | awk 'NF == 2 {d[NR] = $2 - $1; s += d[NR]} END {for (i = 1; i <= NR; i++) if (d[i] != "") printf "%s%d", (i > 1 ? "/" : ""), 100 * d[i] / s + 0.5}')
	r=$(awk -v b=$(( b1 - ${B0[$p]} )) -v w=$W 'BEGIN {printf "%.2f", b / w / 1e9}')
	tot=$(awk -v a=$tot -v b=$r 'BEGIN {printf "%.2f", a + b}')
	echo "port $p $d: $r GB/s sent, FIFO drops $(( f1 - ${F0[$p]} )), ring drops $(( t1 - ${T0[$p]} ))"
done
wait
echo "$WHICH iperf3 -R -Z -l ${LEN:-256K}, $N streams a port: total $tot GB/s sent"
awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; c = $2 + 0; if (c < 8) a += b; else if (c >= 56) z += b; else n += b} END {printf "CPUs busy: rings of a %.0f%%, rings of b %.0f%%, senders (8-55) %.0f%%\n", a / 8, z / 8, n / 48}' /tmp/.iperf-tx-mpstat
