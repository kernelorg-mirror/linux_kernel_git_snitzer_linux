#!/bin/bash
# iperf-tx.sh <a|b|ab> [streams per port] [seconds] - what the wire
# carries OUT of one port, the other, or both at once with no NFS and no
# drives (16 November, round 104: the READ direction): <streams> iperf3
# processes on each port, the senders on nfsd's CPUs (8-55), the client
# receiving (iperf3 -R), LEN (256K) a write.  The senders copy: -Z is
# not passed to a reversed server (iperf3 3.17 writes with write(2), and
# its server refuses -Z), so they pay a copy nfsd's page references do
# not.  The rate is read from the port's
# tx_bytes over the middle of the run.  iperf2port.sh is the other way.
# ZC=1 sends forward from tardis2 with -Z instead.  Ports as twoport.sh
# has them.
. "$(dirname "$0")/common.sh"
WHICH=${1:?a|b|ab}; N=${2:-16}; SECS=${3:-20}
which_ok "$WHICH" || exit 1
declare -A DEV=([a]=${PDEVS[a]} [b]=${PDEVS[b]}) SRV=([a]=${SRVP[a]} [b]=${SRVP[b]}) CLI=([a]=${CLIP[a]} [b]=${CLIP[b]})
PORTS=$(sed 's/./& /g' <<< "$WHICH")
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -i $KEY root@$CLIMGMT"
fifo() { ethtool -S $1 | awk '/port_rx_fifo_tc[0-9]_drop_packets/{s+=$2} END{print s+0}'; }
ringdrop() { ethtool -S $1 | awk '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
rxbytes() { cat /sys/class/net/$1/statistics/tx_bytes; }
# each TX ring's counters (packets, bytes, stops, wakes, TSO sends), to
# take a difference of: bytes over packets is the size of what the stack
# handed the ring
txrings() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_tx_(packets|bytes|tx_stop_q|tx_wake_q|tso|hw_dropped):$/ {sub(/:$/, "", $1); print $1, $2}'; }
txdiff() { awk 'NR == FNR {a[$1] = $2; next} {d = $2 - a[$1]; split($1, k, "_"); r = k[2]; f = $1; sub(/^ring_[0-9]+_tx_/, "", f); v[r, f] = d; R[r] = 1}
	END {for (r = 0; r in R; r++) printf "      tx ring %d: %.2f GB/s, %.0fk sends/s, %.0f KiB a send, TSO %.0f%%, stops %d, wakes %d, hw drops %d\n", r, v[r, "bytes"] / w / 1e9, v[r, "packets"] / w / 1e3, v[r, "packets"] ? v[r, "bytes"] / v[r, "packets"] / 1024 : 0, v[r, "packets"] ? 100 * v[r, "tso"] / v[r, "packets"] : 0, v[r, "tx_stop_q"], v[r, "tx_wake_q"], v[r, "hw_dropped"]}' w=$3 $1 $2; }
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
if [ "${ZC:-0}" = 1 ]; then
	# ZC=1: tardis2 is the iperf3 client and sends forward with -Z
	# (sendfile: page references, large skbs, as nfsd's replies), the
	# client's iperf3 servers receiving.
	for p in $PORTS; do c="$c for i in \$(seq $N); do iperf3 -s -1 -B ${CLI[$p]} -p \$(( 5200 + i )) > /dev/null 2>&1 & done;"; done
	$SSHC "$c wait" < /dev/null &
	sleep 3
	for p in $PORTS; do for i in $(seq $N); do
		taskset -c 8-55 iperf3 -c ${CLI[$p]} -B ${SRV[$p]} -p $(( 5200 + i )) -t $SECS -Z -l ${LEN:-256K} > /dev/null 2>&1 &
	done; done
else
	for p in $PORTS; do for i in $(seq $N); do
		taskset -c 8-55 iperf3 -s -1 -B ${SRV[$p]} -p $(( 5200 + i )) > /dev/null 2>&1 &
	done; done
	for p in $PORTS; do c="$c for i in \$(seq $N); do iperf3 -c ${SRV[$p]} -B ${CLI[$p]} -p \$(( 5200 + i )) -t $SECS -R -l ${LEN:-256K} > /dev/null 2>&1 & done;"; done
	sleep 2
	$SSHC "$c wait" < /dev/null &
fi
W=$(( SECS - 8 )); sleep 5
declare -A F0 T0 B0
for p in $PORTS; do d=${DEV[$p]}; F0[$p]=$(fifo $d); T0[$p]=$(ringdrop $d); B0[$p]=$(rxbytes $d); txrings $d > /tmp/.iperf-tx-$p.0; done
echo 1 > $RG
# PROBE=<file.bt> OUT=<dir>: the probe in the window, into $OUT/<name>.txt,
# and 25 samples 0.2 s apart of every transmit queue's BQL limit and bytes
# in flight, into $OUT/bql.txt
if [ -n "${PROBE:-}" ]; then
	mkdir -p ${OUT:?OUT with PROBE}
	bpftrace -q $PROBE > $OUT/$(basename $PROBE .bt).txt 2>&1 &
	for k in $(seq 25); do
		for d in ${DEV[a]} ${DEV[b]}; do for q in /sys/class/net/$d/queues/tx-*; do
			echo "$k $d ${q##*/} limit $(cat $q/byte_queue_limits/limit) inflight $(cat $q/byte_queue_limits/inflight)"
		done; done
		sleep 0.2
	done > $OUT/bql.txt &
fi
mpstat -P ALL $W 1 > /tmp/.iperf-tx-mpstat 2>&1 &
# the client receiving: its CPUs, and its ports' receive drops (dropped
# and missed, as ip -s link counts them)
cdrops() { $SSHC "for d in ens3np0 ens6np0; do ip -s -s link show dev \$d; done" < /dev/null | awk '/RX:/ {getline; d += $4; m += $5} END {print d + m}'; }
CD0=$(cdrops)
$SSHC "mpstat -P ALL $W 1" > /tmp/.iperf-tx-cmpstat 2>&1 < /dev/null &
sleep $W
tot=0
for p in $PORTS; do
	d=${DEV[$p]}; f1=$(fifo $d); t1=$(ringdrop $d); b1=$(rxbytes $d); txrings $d > /tmp/.iperf-tx-$p.1
	r=$(awk -v b=$(( b1 - ${B0[$p]} )) -v w=$W 'BEGIN {printf "%.2f", b / w / 1e9}')
	tot=$(awk -v a=$tot -v b=$r 'BEGIN {printf "%.2f", a + b}')
	echo "port $p $d: $r GB/s sent, FIFO drops $(( f1 - ${F0[$p]} )), ring drops $(( t1 - ${T0[$p]} ))"
	txdiff /tmp/.iperf-tx-$p.0 /tmp/.iperf-tx-$p.1 $W
done
wait
echo "$WHICH iperf3 $([ "${ZC:-0}" = 1 ] && echo "-Z, tardis2 sending" || echo -R) -l ${LEN:-256K}, $N streams a port: total $tot GB/s sent"
awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; c = $2 + 0; if (c < 8) a += b; else if (c >= 56) z += b; else n += b} END {printf "CPUs busy: rings of a %.0f%%, rings of b %.0f%%, senders (8-55) %.0f%%\n", a / 8, z / 8, n / 48}' /tmp/.iperf-tx-mpstat
awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; n++; t += b; so += $8; if (b > 90) h++; if (b > m) m = b} END {printf "client CPUs: mean %.0f%% busy (softirq %.0f%%), %d over 90%%, busiest %.0f%%; receive drops %d\n", t / n, so / n, h, m, d}' d=$(( $(cdrops) - CD0 )) /tmp/.iperf-tx-cmpstat
