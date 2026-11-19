# stages.awk: perf script -F comm,cpu,ip,sym on stdin; -v reads= hz= out=.
# Each sample to one stage, the first frame from the leaf up that a rule
# names; idle samples left out.  Prints us a READ by stage for nfsd's
# CPUs and the rings', writes out/{nfsd,rings}.folded and leaf-*.txt.
function grp(c) { return (c >= 8 && c <= 55) ? "nfsd" : "rings" }
function stage(f) {
	if (f ~ /^(cpu_do_idle|arch_cpu_idle|default_idle_call|cpuidle_enter_state|psci_cpu_suspend|do_idle|cpu_startup_entry)$/) return "idle"
	if (f ~ /^xeu_/) return "NIC driver (xeu)"
	if (f ~ /^(nvme_|blk_|bio_|__bio|submit_bio|iomap_dio|__iomap_dio|iomap_iter|dio_)/) return "block, NVMe, iomap"
	if (f ~ /^(xfs_|__xfs)/) return "XFS"
	if (f ~ /^(napi_consume_skb|__kfree_skb|kfree_skb|sk_skb_reason_drop|consume_skb|skb_release|tcp_wfree|__napi_kfree|napi_skb_cache|skb_free_head|page_pool_)/) return "skb and page free"
	if (f ~ /^(tcp_ack|tcp_clean_rtx_queue|tcp_rate_|tcp_rtt_|tcp_cong|bictcp|cubictcp|tcp_newly_delivered)/) return "TCP ACK processing"
	if (f ~ /^(tcp_v4_rcv|tcp_rcv_established|tcp_v4_do_rcv|tcp_data_queue|tcp_queue_rcv|tcp_event_data_recv|tcp_add_backlog|__release_sock|release_sock|ip_rcv|ip_local_deliver|ip_list_rcv|__netif_receive|netif_receive|process_backlog|__napi_poll|napi_gro|gro_|dev_gro|skb_gro|inet_gro|tcp4_gro|tcp_gro)/) return "TCP/IP receive"
	if (f ~ /^(tcp_sendmsg|tcp_write_xmit|tcp_push|__tcp_push|tcp_transmit_skb|__tcp_transmit|ip_queue_xmit|__ip_queue_xmit|ip_finish|ip_output|ip_local_out|__dev_queue_xmit|dev_queue_xmit|__dev_xmit_skb|sch_direct_xmit|dev_hard_start_xmit|qdisc_|codel|tcp_send_ack|__tcp_send_ack|sk_reset_timer|tcp_schedule_loss|tcp_small_queue|tcp_tsq|tcp_tasklet)/) return "TCP/IP send"
	if (f ~ /^(tcp_recvmsg|tcp_read_sock|svc_tcp_recvfrom|svc_tcp_read|svc_tcp_|svc_sock_|xs_|svc_recv)/) return "RPC receive (socket)"
	if (f ~ /^(svc_tcp_sendto|svc_tcp_send|svc_sendto|xprt_sock_sendmsg|svc_rtc_send|svc_rtc_drain_sends)/) return "RPC send (socket)"
	if (f ~ /^(fh_verify|__fh_verify|nfsd_fh_borrow|nfsd_set_fh_dentry|exp_find|rqst_exp|nfsd_cache_lookup|nfsd_cache_update)/) return "fh_verify, export, DRC"
	if (f ~ /^(svc_authenticate|svcauth|svc_set_client|unix_gid|auth_unix|put_cred|prepare_creds|override_creds|revert_creds|nfsd_setuser|free_uid)/) return "auth and creds"
	if (f ~ /^(nfsd3_|nfsd_read|nfsd_finish_read|nfsd_iter_read|nfsd_direct|nfsd_file|nfs3svc_|svcxdr_|xdr_|nfsd_dispatch|nfsd_|nfs_dio)/) return "nfsd READ and XDR"
	if (f ~ /^(svc_|rpc_|sunrpc)/) return "sunrpc (rtc, dispatch)"
	if (f ~ /^(schedule|__schedule|try_to_wake_up|ttwu|finish_task_switch|pick_next|dequeue_task|enqueue_task|wake_up|__wake_up)/) return "scheduler, wakeups"
	if (f ~ /^(process_one_work|worker_thread|__queue_work|queue_work_on|kick_pool|insert_work)/) return "workqueue"
	if (f ~ /^(handle_softirqs|__do_softirq|do_softirq|net_rx_action|net_tx_action|__local_bh_enable|irq_exit)/) return "softirq (other)"
	if (f ~ /^(gic_|handle_irq|__handle_irq|el1_interrupt|el0_interrupt|generic_handle|handle_fasteoi|ipi_handler|do_handle_IPI|hrtimer_interrupt|arch_timer)/) return "IRQ, IPI, timer"
	return ""
}
function flush(   i, s, st, k) {
	if (!nf) return
	st = ""
	# idle: under do_idle with no interrupt taken (an interrupt in idle
	# is work, staged as any other)
	for (i = 1; i <= nf; i++) {
		if (fr[i] ~ /^(el1_interrupt|el1h_64_irq|handle_softirqs|do_handle_IPI|gic_handle_irq)$/) break
		if (fr[i] == "do_idle") { st = "idle"; break }
	}
	if (st == "")
		for (i = 1; i <= nf; i++) { s = stage(fr[i]); if (s != "") { st = s; break } }
	if (st == "") st = "other"
	if (st != "idle") {
		g = grp(cpu); n[st, g]++; tot[g]++; stg[st] = 1
		leaf[fr[1], g]++
		k = ""; for (i = nf; i >= 1; i--) k = k (k == "" ? "" : ";") fr[i]
		fold[g, comm ";" k]++
	}
	nf = 0
}
/^[^ \t]/ { flush(); m = match($0, /\[[0-9]+\]/); cpu = substr($0, RSTART + 1, RLENGTH - 2) + 0
	comm = substr($0, 1, RSTART - 1); sub(/[ \t]+$/, "", comm); sub(/^[ \t]+/, "", comm); gsub(/[ ;]/, "_", comm); next }
/^[ \t]+[0-9a-f]+ / { s = $2; sub(/\+0x[0-9a-f]+$/, "", s); fr[++nf] = s; next }
/^$/ { flush() }
END {
	flush()
	u = 1e6 / hz / reads
	printf "READs in the window %d; us a READ from samples: nfsd CPUs %.2f, rings %.2f, all %.2f\n\n", reads, tot["nfsd"] * u, tot["rings"] * u, (tot["nfsd"] + tot["rings"]) * u
	printf "%-28s %8s %8s %8s %6s\n", "stage", "nfsd us", "rings us", "all us", "share"
	for (s in stg) order[s] = n[s, "nfsd"] + n[s, "rings"]
	PROCINFO["sorted_in"] = "@val_num_desc"
	for (s in order) printf "%-28s %8.2f %8.2f %8.2f %5.1f%%\n", s, n[s, "nfsd"] * u, n[s, "rings"] * u, order[s] * u, 100 * order[s] / (tot["nfsd"] + tot["rings"])
	delete order
	for (k in leaf) { split(k, a, SUBSEP); L[a[2]][a[1]] = leaf[k] }
	for (g in L) { f = out "/leaf-" g ".txt"; c = 0
		for (s in L[g]) if (c++ < 60) printf "%7.3f us %5.2f%%  %s\n", L[g][s] * u, 100 * L[g][s] / tot[g], s > f }
	PROCINFO["sorted_in"] = ""
	for (k in fold) { split(k, a, SUBSEP); print a[2], fold[k] > (out "/" a[1] ".folded") }
}
