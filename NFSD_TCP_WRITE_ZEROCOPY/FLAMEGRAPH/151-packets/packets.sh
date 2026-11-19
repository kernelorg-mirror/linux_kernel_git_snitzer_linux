#!/bin/bash
# packets.sh <n> [iodepth]: FLAMEGRAPH round 151, what crosses the wire
# and the stacks for each 4 KiB READ (Mike, 19 November: "yes, run the
# packet-count measurement as round 151").  Round 150 put some 9 of the
# server's 15.8 us a READ in the network; this counts, per READ, the
# packets, TCP segments, pure ACKs, delayed ACKs, NIC interrupts and
# NET_RX / NET_TX softirqs on each host.  One settled-iops.sh run of
# nfs4k.sh <n>:<iodepth> (96), the client on its updates/ stack, and
# from the load's eighth second two snapshots 8 s apart on both hosts
# (nstat absolute, the ports' link counters, the NIC's interrupts,
# /proc/softirqs, the RPC and READ counts).  Only reads; into
# $L/t2-r<n>/: pk-{srv,cli}-{0,1}.txt and packets.txt.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?packets.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; P=../FLAMEGRAPH/151-packets
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
# snap <ifs> <irq pattern> <rpc file>: one "key value" a line
SNAP='snap() {
	for i in $1; do ip -s -s link show $i | awk -v i=$i "/RX:/{getline; print \"link_rx_pkts\", \$2} /TX:/{getline; print \"link_tx_pkts\", \$2}"; done
	awk -v p="$2" "NR>1 && \$NF ~ p { for (k = 2; k <= NF; k++) if (\$k ~ /^[0-9]+\$/) s += \$k } END { print \"nic_irqs\", s }" /proc/interrupts
	awk "/NET_RX|NET_TX/ { s = 0; for (k = 2; k <= NF; k++) s += \$k; sub(\":\", \"\", \$1); print \"softirq_\" \$1, s }" /proc/softirqs
	awk "/^rpc / { print \"rpc_calls\", \$2 } /^proc3 / { print \"nfs3_read\", \$9 }" $3
	nstat -asz | awk "NR>1 { print \$1, \$2 }"
}'
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	for k in 0 1; do
		$SSHC "$SNAP; snap 'ens3np0 ens6np0' mlx5 /proc/net/rpc/nfs" > $O/pk-cli-$k.txt &
		bash -c "$SNAP; snap 'enp161s0f0 enp164s0f0' enp16 /proc/net/rpc/nfsd" > $O/pk-srv-$k.txt
		wait
		[ $k = 0 ] && sleep 8
	done
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
awk -f $P/packets.awk $O/pk-srv-0.txt $O/pk-srv-1.txt $O/pk-cli-0.txt $O/pk-cli-1.txt | tee $O/packets.txt
exit $rc
