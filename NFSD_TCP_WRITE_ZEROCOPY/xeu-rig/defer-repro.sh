#!/bin/bash
# defer-repro.sh <out dir> [a|b] - for Xsight: the kernel's interrupt
# deferral stops an xeu port (the note's 3a).  Plain TCP into the port, no
# NFS and no drives (iperf2port.sh, receivers discarding); once the load is
# up, napi_defer_hard_irqs=2 and gro_flush_timeout=50000 are written to the
# port, and each second for 10 s the script records the rings' frames, the
# port's Rx interrupts and the NAPI state (ynl netdev napi-get).  Then both
# are written back to 0 and the same is recorded for 8 s, the client is
# pinged over the port, and the port is taken down and up and pinged again.
# settled.sh apply ends it.  On 15 October (build 474) the frames fell from
# 3 million a second to a handful within 2 s and did not come back at 0;
# only the down and up did.
#   <out>/defer.txt        the record, one line a second
#   <out>/napi-*.txt       ynl's napi-get before, deferred, and after 0
#   <out>/iperf2port.txt   the load's own report
cd "$(dirname "$0")" || exit 1
. ./common.sh
O=${1:?out dir}; P=${2:-a}; mkdir -p $O
D=${PDEVS[$P]}; N=/sys/class/net/$D; CI=${CLIP[$P]}
IDX=$(cat $N/ifindex)
YNL="python3 $RIG/../../tools/net/ynl/pyynl/cli.py --no-schema --spec $RIG/../../Documentation/netlink/specs/netdev.yaml"
say() { echo "$(date +%T) $*" | tee -a $O/defer.txt; }
frames() { ethtool -S $D | awk '/^ *ring_[0-9]+_rx_packets:/{f+=$2} END{print f+0}'; }
irqs() { grep -E "$D-Rx-" /proc/interrupts | awk '{s=0; for (i=2; i<=NF; i++) if ($i ~ /^[0-9]+$/) s+=$i; t+=s} END{print t+0}'; }
napi() { $YNL --dump napi-get --json "{\"ifindex\": $IDX}" > $O/napi-$1.txt 2>&1; }
napi_line() { python3 -I -c 'import ast,sys; l=ast.literal_eval(open(sys.argv[1]).read()); print(" ".join("%s:%s/%s" % (n["id"], n.get("defer-hard-irqs"), n.get("gro-flush-timeout")) for n in l))' $O/napi-$1.txt 2>/dev/null; }
ping_port() { ping -c 2 -W 1 -I $D $CI > /dev/null 2>&1 && echo answers || echo "NO ANSWER"; }
sample() {	# sample <seconds> <label>: frames/s and Rx interrupts/s, once a second
	local i f0 f1 q0 q1
	f0=$(frames); q0=$(irqs)
	for i in $(seq 1 $1); do
		sleep 1; f1=$(frames); q1=$(irqs)
		say "$2 +${i}s frames/s $((f1 - f0)) rx_irqs/s $((q1 - q0))"
		f0=$f1; q0=$q1
	done
}

: > $O/defer.txt
say "kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), port $P $D"
./settled.sh apply > $O/apply-start.txt 2>&1 || { say "not at the settled setting: $(grep -E 'BAD|FAILED' $O/apply-start.txt | head -3)"; exit 1; }
napi before; say "napi before (id:defer/gro_flush): $(napi_line before)"
# A stopped port can leave the senders waiting: its EXIT trap ends them.
MODE=trunc timeout 150 ./iperf2port.sh $P 16 40 > $O/iperf2port.txt 2>&1 & LP=$!
f0=$(frames)
while :; do	# the load is up at a million frames a second
	sleep 2; f1=$(frames)
	[ $(( (f1 - f0) / 2 )) -gt 1000000 ] && break
	f0=$f1; kill -0 $LP 2>/dev/null || { say "the load ended before it came up"; exit 1; }
done
say "load up: $(( (f1 - f0) / 2 )) frames/s"
sample 2 load
echo 2 > $N/napi_defer_hard_irqs; echo 50000 > $N/gro_flush_timeout
napi deferred; say "set napi_defer_hard_irqs=$(cat $N/napi_defer_hard_irqs) gro_flush_timeout=$(cat $N/gro_flush_timeout); napi: $(napi_line deferred)"
sample 10 deferred
echo 0 > $N/napi_defer_hard_irqs; echo 0 > $N/gro_flush_timeout
napi zero; say "both back to 0; napi: $(napi_line zero)"
sample 8 zero
say "ping the client over the port: $(ping_port)"
wait $LP; say "load ended rc=$?: $(grep -E 'total' $O/iperf2port.txt | head -1)"
dmesg -T | grep -E "xeu|$D" | tail -3 >> $O/defer.txt
ip link set $D down; sleep 1; ip link set $D up; sleep 5
say "after a down and up: ping $(ping_port)"
./settled.sh apply > $O/apply-end.txt 2>&1; say "end: $(tail -1 $O/apply-end.txt)"
