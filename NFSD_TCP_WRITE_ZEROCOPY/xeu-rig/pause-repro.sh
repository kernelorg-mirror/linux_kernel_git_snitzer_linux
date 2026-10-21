#!/bin/bash
# pause-repro.sh <out dir> [a|b] - for Xsight: an xeu port does not send
# pause frames (the note's 2b).  The client's port is set to obey pause
# (ethtool -A rx on), and the server's port is asked to send it two ways:
#   tx       ethtool -A <port> tx on, the port left open
#   rxtx     ethtool -A <port> rx on tx on with the port down and up first,
#            so it opens with pause set, as on 11 October
# iperf2port.sh's settled.sh place sets pause off and its audit wants it
# off at both ends, so pause is set, at both ends, once the load is up; for
# rxtx that is after the open with it set (place turns it off without
# reopening the port).
# Each case is a load that overflows the RX FIFO: plain TCP into the port
# with the receivers copying (iperf2port.sh MODE=copy) and copybench loading
# memory on the other port's ring CPUs, which with nothing else running
# carries the FIFO past xoff (12288 lines; X2 of XSIGHT-FOLLOWUP.md saw
# 15,000).  Each second: the FIFO's fill, its drops and its flow-control
# count from ethtool -S (rx_port_stats), and the pause frames the client's
# port received.  Pause goes back off at both ends, and settled.sh apply
# ends it.  On 11 October no pause frame reached the client in four runs.
#   <out>/pause.txt                    the record
#   <out>/<case>.samples               one line a second
#   <out>/<case>-iperf2port.txt        the load's own report
cd "$(dirname "$0")" || exit 1
. ./common.sh
O=${1:?out dir}; P=${2:-a}; mkdir -p $O
D=${PDEVS[$P]}
declare -A CDEVS=([a]=ens3np0 [b]=ens6np0); CD=${CDEVS[$P]}
CB=$O/copybench; CBCPU=$([ $P = a ] && echo 56 || echo 0)
say() { echo "$(date +%T) $*" | tee -a $O/pause.txt; }
fifo() { ethtool -S $D | awk '/port_rx_fifo_tc0_lines:/{l=$2} /port_rx_fifo_tc0_drop_packets:/{d=$2} /port_rx_fifo_fc_lines:/{f=$2} END{print l+0, d+0, f+0}'; }
cpause() { $SSHC "ethtool -S $CD | awk '/rx_pause_ctrl_phy:/{print \$2}'" < /dev/null; }
pause_state() { echo "server $(ethtool -a $D | awk '/^RX:/{r=$2} /^TX:/{t=$2} END{print r "/" t}') client $($SSHC "ethtool -a $CD" < /dev/null | awk '/^RX:/{r=$2} /^TX:/{t=$2} END{print r "/" t}')"; }
frames() { ethtool -S $D | awk '/^ *ring_[0-9]+_rx_packets:/{f+=$2} END{print f+0}'; }
run() {	# run <case> <server ethtool -A args>: the load, pause set once it is up
	local c=$1 lp cb p0 p1 i s f0 f1; shift
	MODE=copy timeout 150 ./iperf2port.sh $P 16 40 > $O/$c-iperf2port.txt 2>&1 & lp=$!
	f0=$(frames)
	while :; do	# the load is up at a million frames a second
		sleep 2; f1=$(frames)
		[ $(( (f1 - f0) / 2 )) -gt 1000000 ] && break
		f0=$f1; kill -0 $lp 2>/dev/null || { say "$c: the load ended before it came up"; return 1; }
	done
	ethtool -A $D "$@"; $SSHC "ethtool -A $CD rx on" < /dev/null
	say "$c: load up; pause $(pause_state)"
	$CB stp 8 $CBCPU 28 > $O/$c-copybench.txt 2>&1 & cb=$!
	p0=$(cpause); : > $O/$c.samples
	for i in $(seq 1 28); do
		sleep 1; s=$(fifo); p1=$(cpause)
		echo "$(date +%T) +${i}s fifo_lines/drops/fc_lines $s client_rx_pause $((p1 - p0))" >> $O/$c.samples
	done
	wait $cb $lp
	say "$c: $(grep -E '^port' $O/$c-iperf2port.txt | head -1) | fifo lines at most $(awk '{if ($4 > m) m = $4} END {print m+0}' $O/$c.samples) | client pause frames received $(( $(cpause) - p0 ))"
}

: > $O/pause.txt
say "kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), port $P $D, client $CD"
./settled.sh apply > $O/apply-start.txt 2>&1 || { say "not at the settled setting: $(grep -E 'BAD|FAILED' $O/apply-start.txt | head -3)"; exit 1; }
[ -x $CB ] || gcc -O2 -pthread -o $CB copybench.c || { say "copybench build failed"; exit 1; }

run tx tx on
ethtool -A $D rx off tx off; $SSHC "ethtool -A $CD rx off tx off" < /dev/null

# Not settled.sh place after the open: it sets pause off.  Port a's ring
# interrupts are on 0-7 settled, the driver's own spread, so the open
# leaves them there.
ethtool -A $D rx on tx on; ip link set $D down; sleep 1; ip link set $D up; sleep 5
say "rxtx: opened with pause $(pause_state)"
run rxtx rx on tx on

ethtool -A $D rx off tx off; $SSHC "ethtool -A $CD rx off tx off" < /dev/null
say "pause off: $(pause_state)"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
