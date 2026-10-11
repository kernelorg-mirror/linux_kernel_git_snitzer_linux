#!/bin/bash
# where.sh <out prefix> [seconds] - during a fast cell, what each end is
# doing: every CPU's time on the server and on the client, the port's
# receive per ring, a flat profile of the server, and the client's view of
# each connection (ss -ti: how long it was busy, and how much of that it
# waited on the server's window or on its own send buffer).
. "$(dirname "$0")/common.sh"
O=${1:?out prefix}; N=${2:-10}
$SSHC "ss -tin dst $SRVIP" > $O.ss0 2>&1
r0=$(ethtool -S $D | grep -E 'ring_[0-9]+_rx_(packets|bytes)'); t0=$(tail_drops); s0=$(date +%s.%N)
mpstat -P ALL $N 1 > $O.srv-mpstat 2>&1 &
$SSHC "mpstat -P ALL $N 1" > $O.cli-mpstat 2>&1 &
perf record -a -F 199 -o $O.data -- sleep $N > /dev/null 2>&1
wait
r1=$(ethtool -S $D | grep -E 'ring_[0-9]+_rx_(packets|bytes)'); s1=$(date +%s.%N)
# Over the time between the two readings, not $N: perf's start and stop and
# the wait for the client's mpstat are inside it, and until 2026-10-10 the
# rates here were divided by $N and came out a tenth or more too high (the
# rings' shares were right).
paste <(echo "$r0") <(echo "$r1") | awk -v n=$(echo "$s1 $s0" | awk '{print $1 - $2}') '{printf "%s %.0f /s\n", $1, ($4-$2)/n}' > $O.rings
echo "rings read $(echo "$s1 $s0" | awk '{printf "%.2f", $1 - $2}') s apart" >> $O.rings
echo "tail drops in $N s: $(( $(tail_drops) - t0 ))" >> $O.rings
$SSHC "ss -tin dst $SRVIP" > $O.ss1 2>&1
perf report -i $O.data --stdio --sort symbol 2>/dev/null | grep -v '^#' | grep -v '^$' | head -45 > $O.profile; rm -f $O.data
echo "== server CPUs by busy (usr sys irq soft idle), top 12 of $(nproc)"; awk '/^Average/ && $2 ~ /^[0-9]+$/ {printf "cpu%-3s usr %5.1f sys %5.1f irq %4.1f soft %5.1f idle %5.1f\n",$2,$3,$5,$7,$8,$12}' $O.srv-mpstat | sort -k11n | head -12
echo "== client CPUs, top 12"; awk '/^Average/ && $2 ~ /^[0-9]+$/ {printf "cpu%-3s usr %5.1f sys %5.1f irq %4.1f soft %5.1f idle %5.1f\n",$2,$3,$5,$7,$8,$12}' $O.cli-mpstat | sort -k11n | head -12
awk '/^Average/ && $2=="all"{print "client all:", $0}' $O.cli-mpstat; awk '/^Average/ && $2=="all"{print "server all:", $0}' $O.srv-mpstat
echo "== rings"; cat $O.rings
echo "== client connections (ss -ti), the limits it reports"; grep -o -E 'busy:[0-9]+ms|rwnd_limited:[0-9]+ms\([0-9.]+%\)|sndbuf_limited:[0-9]+ms\([0-9.]+%\)|cwnd:[0-9]+|delivery_rate [0-9.]+[MG]bps|pacing_rate [0-9.]+[MG]bps' $O.ss1 | paste - - - - - - | head -20
echo "== profile"; head -25 $O.profile
