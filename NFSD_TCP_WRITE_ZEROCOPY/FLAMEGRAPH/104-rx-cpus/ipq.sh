#!/bin/bash
# ipq.sh <label>: each connection's backlog under iperf3 -Z (iperf-tx.sh ab
# with ZC=1, 16 streams a port), to set beside NFS's (round 104, 17
# November, Mike: "the nature of the backlog for each connection with
# iperf3, in contrast the backlog for NFS").  In the window, ten samples
# 0.5 s apart of the server's sending sockets (Send-Q, skmem t) and the
# client's receiving sockets (Recv-Q), ports 5201-5216.
H=$(cd "$(dirname "$0")" && pwd); O=/var/tmp/zc-run-logs/ipq-${1:?label}; mkdir -p $O
HD=$H; . $H/../../xeu-rig/common.sh; H=$HD
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -i $KEY root@$CLIMGMT"
cd $H/../../xeu-rig || exit 1
ZC=1 ./iperf-tx.sh ab 16 30 > $O/iperf.out 2>&1 &
sleep 12
for k in $(seq 10); do
	ss -tmH state established '( sport >= :5201 and sport <= :5216 ) or ( dport >= :5201 and dport <= :5216 )' |
		awk -v k=$k '$1 ~ /^[0-9]+$/ {q=$2} /skmem/ {match($0, /,t[0-9]+/); print "srv", k, q, substr($0, RSTART + 2, RLENGTH - 2)}' >> $O/q.txt
	$SSHC "ss -tnH state established '( sport >= :5201 and sport <= :5216 ) or ( dport >= :5201 and dport <= :5216 )'" < /dev/null |
		awk -v k=$k '{print "cli", k, $1}' >> $O/q.txt
	sleep 0.5
done
wait
grep total $O/iperf.out
awk '$1 == "srv" {s += $3; t += $4; ns++} $1 == "cli" {c += $3; nc++}
	END {printf "per connection: server Send-Q %.2f MB, below TCP (skmem t) %.2f MB; client Recv-Q %.2f MB (%d and %d samples)\n", s / ns / 1e6, t / ns / 1e6, c / nc / 1e6, ns, nc}' $O/q.txt
