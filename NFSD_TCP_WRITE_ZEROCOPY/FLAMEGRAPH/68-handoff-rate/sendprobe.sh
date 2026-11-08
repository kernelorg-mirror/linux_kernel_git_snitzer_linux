#!/bin/bash
# sendprobe.sh SECONDS (run on the client): which context sends the NFS
# (counts are samples, one transmit in 50; read the shares)
# writes' packets.  Samples one net:net_dev_start_xmit in 50 on all CPUs with
# call chains, and counts each sample by NIC (ens3np0 is port a, on node 0;
# ens6np0 port b, on node 1), by whether its CPU is on the NIC's node (node 0
# is CPUs 0-23, node 1 24-47), and by the path that reached the transmit:
#   sendmsg    tcp_sendmsg: the writer's own call (fio, or rpciod/xprtiod)
#   softirq    net_rx_action: clocked by an ACK in the receive softirq
#   backlog    __release_sock: clocked by an ACK queued while a task held
#              the socket, processed as it let go (in that task's context)
#   ack        tcp_rcv_established reached any other way
#   release    tcp_release_cb: sent when the socket's lock was let go
# and, for the far node's samples, the task they ran in (comm).
#   other      anything else (timers, tcp_tsq_handler, ...)
S=${1:-10}
O=/var/tmp/sendprobe.$$
perf record -q -e net:net_dev_start_xmit -c 50 -a -g -o $O.data -- sleep $S 2>/dev/null
perf script -i $O.data 2>/dev/null | awk -v RS= '
	{
		if (!match($0, /dev=[a-z0-9]+/)) next
		dev = substr($0, RSTART + 4, RLENGTH - 4)
		if (dev != "ens3np0" && dev != "ens6np0") next
		match($0, /\[[0-9]+\]/); cpu = substr($0, RSTART + 1, RLENGTH - 2) + 0
		node = cpu < 24 ? 0 : 1
		near = (dev == "ens3np0" ? node == 0 : node == 1) ? "near" : "far"
		path = "other"
		if ($0 ~ /tcp_release_cb/) path = "release"
		else if ($0 ~ /net_rx_action/) path = "softirq"
		else if ($0 ~ /__release_sock/) path = "backlog"
		else if ($0 ~ /tcp_rcv_established/) path = "ack"
		else if ($0 ~ /tcp_sendmsg/) path = "sendmsg"
		n[dev " " near " " path]++; t[dev]++
		if (near == "far") { split($0, w, " "); comm = w[1]; sub(/\/[0-9]+$/, "", comm); f[dev " " comm " " path]++ }
	}
	END {
		for (k in n) { split(k, a, " "); printf "%s %-4s %-8s %10d %5.1f%%\n", a[1], a[2], a[3], n[k], 100 * n[k] / t[a[1]] }
		for (k in f) { split(k, a, " "); printf "far task %s %-16s %-8s %10d %5.2f%%\n", a[1], a[2], a[3], f[k], 100 * f[k] / t[a[1]] }
	}' | sort
rm -f $O.data
