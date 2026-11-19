# packets.awk srv-0 srv-1 cli-0 cli-1: the two snapshots' differences,
# each divided by the server's NFSv3 READs in the window.
FNR == 1 { f++ }
{ h = (f <= 2) ? "srv" : "cli"; if (f % 2) a[h, $1] += $2; else b[h, $1] += $2; key[$1] = 1 }
END {
	printf "CPUs: server ring %d other %d; client ring %d other %d\n", b["srv", "cpus_ring"], b["srv", "cpus_other"], b["cli", "cpus_ring"], b["cli", "cpus_other"]
	r = b["srv", "nfs3_read"] - a["srv", "nfs3_read"]
	if (r <= 0) { print "no READs in the window"; exit 1 }
	printf "READs in the window: %d (%.2f M a second over 8 s)\n", r, r / 8e6
	printf "%-34s %10s %10s\n", "per READ", "server", "client"
	split("nfs3_read rpc_calls link_rx_pkts link_tx_pkts TcpInSegs TcpOutSegs TcpExtTCPPureAcks TcpExtDelayedACKs TcpExtTCPHPAcks TcpExtTCPHPHits TcpExtTCPDelivered TcpRetransSegs TcpExtTCPAutoCorking TcpExtTCPBacklogCoalesce TcpExtTCPOrigDataSent nic_irqs softirq_NET_RX softirq_NET_TX softirq_NET_RX_ring softirq_NET_RX_other softnet_processed_ring softnet_processed_other softnet_rps_ipis_ring softnet_rps_ipis_other softnet_squeeze_ring softnet_squeeze_other", o, " ")
	for (i = 1; i in o; i++) { k = o[i]
		printf "%-34s %10.3f %10.3f\n", k, (b["srv", k] - a["srv", k]) / r, (b["cli", k] - a["cli", k]) / r }
}
