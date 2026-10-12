#!/bin/bash
# iperf2port.sh <a|b|ab> [streams per port] [seconds] - what the wire
# carries into one port, the other, or both at once with no NFS and no
# drives: <streams> iperf3 processes to each port, the receivers on nfsd's
# CPUs (8-55).  The rate is read from the rings' byte counts over the
# middle of the run, with the port's FIFO and ring drops.
# MODE=trunc: the receivers discard without copying; MODE=copy: the same
# tool with the copy, to compare with it.
# Ports as twoport.sh has them.
. "$(dirname "$0")/common.sh"
WHICH=${1:?a|b|ab}; N=${2:-16}; SECS=${3:-20}
declare -A DEV=([a]=enp161s0f0 [b]=enp164s0f0) SRV=([a]=192.168.1.211 [b]=192.168.0.211) CLI=([a]=192.168.1.106 [b]=192.168.0.106)
PORTS=$(sed 's/./& /g' <<< "$WHICH")
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=10 -i $KEY root@192.168.1.106"
fifo() { ethtool -S $1 | awk '/port_rx_fifo_tc[0-9]_drop_packets/{s+=$2} END{print s+0}'; }
ringdrop() { ethtool -S $1 | awk '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
rxbytes() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}'; }
shares() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{print $2}' | tr '\n' ' '; }
trap 'pkill -x iperf3; pkill -f "[w]ire-rcv[.]py"; $SSHC "pkill -x iperf3; pkill -f "[w]ire-snd[.]py"" < /dev/null; echo N > /sys/module/xeu/parameters/rx_port_stats' EXIT
echo Y > /sys/module/xeu/parameters/rx_port_stats
# Each port's ring interrupts on its own CPUs, as twoport.sh has them.
declare -A IRQ0=([a]=0 [b]=56)
for p in $PORTS; do "$(dirname "$0")/ring-irqs.sh" ${DEV[$p]} ${IRQ0[$p]} > /dev/null; done
pkill -x iperf3; sleep 1
c=""
if [ -n "${MODE:-}" ]; then
	# MODE=copy|trunc: python receivers and senders (wire-rcv.py,
	# wire-snd.py), one process a port and a thread a stream.  trunc
	# has the kernel discard what arrives without copying it.
	pkill -f "[w]ire-rcv[.]py"; scp -q -o BatchMode=yes -i $KEY $RIG/wire-snd.py root@192.168.1.106:/var/tmp/wire-snd.py
	for p in $PORTS; do
		taskset -c 8-55 python3 $RIG/wire-rcv.py ${SRV[$p]} 5300 $N $MODE > /tmp/.wire-rcv-$p.out 2>&1 &
		c="$c python3 /var/tmp/wire-snd.py ${SRV[$p]} 5300 ${CLI[$p]} $N $SECS &"
	done
else
	for p in $PORTS; do for i in $(seq $N); do
		taskset -c 8-55 iperf3 -s -1 -B ${SRV[$p]} -p $(( 5200 + i )) > /dev/null 2>&1 &
	done; done
	for p in $PORTS; do c="$c for i in \$(seq $N); do iperf3 -c ${SRV[$p]} -B ${CLI[$p]} -p \$(( 5200 + i )) -t $SECS -l 1M > /dev/null 2>&1 & done;"; done
fi
sleep 2
$SSHC "$c wait" < /dev/null &
W=$(( SECS - 8 )); sleep 5
declare -A F0 T0 B0 S0
for p in $PORTS; do d=${DEV[$p]}; F0[$p]=$(fifo $d); T0[$p]=$(ringdrop $d); B0[$p]=$(rxbytes $d); S0[$p]=$(shares $d); done
echo 1 > $RG
mpstat -P ALL $W 1 > /tmp/.iperf2port-mpstat 2>&1 &
sleep $W
tot=0
for p in $PORTS; do
	d=${DEV[$p]}; f1=$(fifo $d); t1=$(ringdrop $d); b1=$(rxbytes $d); s1=$(shares $d)
	sh=$(paste <(tr ' ' '\n' <<< "${S0[$p]}") <(tr ' ' '\n' <<< "$s1") | awk 'NF == 2 {d[NR] = $2 - $1; s += d[NR]} END {for (i = 1; i <= NR; i++) if (d[i] != "") printf "%s%d", (i > 1 ? "/" : ""), 100 * d[i] / s + 0.5}')
	r=$(awk -v b=$(( b1 - ${B0[$p]} )) -v w=$W 'BEGIN {printf "%.2f", b / w / 1e9}')
	tot=$(awk -v a=$tot -v b=$r 'BEGIN {printf "%.2f", a + b}')
	echo "port $p $d: $r GB/s, FIFO drops $(( f1 - ${F0[$p]} )), ring drops $(( t1 - ${T0[$p]} )), rings $sh%"
done
wait
echo "$WHICH ${MODE:-iperf3} $N streams a port: total $tot GB/s on the wire; region pages in use at most $(( $(reg pages) - $(reg min_free) ))"
awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; c = $2 + 0; if (c < 8) a += b; else if (c >= 56) z += b; else n += b} END {printf "CPUs busy: rings of a %.0f%%, rings of b %.0f%%, receivers (8-55) %.0f%%\n", a / 8, z / 8, n / 48}' /tmp/.iperf2port-mpstat
