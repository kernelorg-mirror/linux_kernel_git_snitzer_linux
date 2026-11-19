# cstages.awk: the client's perf script -F comm,cpu,ip,sym,dso on stdin;
# -v reads= hz= out=.  Each sample to one stage, the first frame from the
# leaf up that a rule names; idle left out.  Groups by command.
function grp(c) { return c ~ /^fio/ ? "fio" : c ~ /^kworker/ ? "kworker" : c ~ /^swapper/ ? "idle-irq" : "other" }
function stage(f, d) {
	if (d !~ /^\[/) return "user space (fio, liburing)"
	if (f ~ /^mlx5/) return "NIC driver (mlx5)"
	if (f ~ /^(napi_consume_skb|__kfree_skb|kfree_skb|sk_skb_reason_drop|consume_skb|skb_release|tcp_wfree|__napi_kfree|napi_skb_cache|skb_free_head|page_pool_|skb_attempt_defer)/) return "skb and page free"
	if (f ~ /^(tcp_ack|tcp_clean_rtx_queue|tcp_rate_|tcp_rtt_|tcp_cong|bictcp|cubictcp|tcp_newly_delivered)/) return "TCP ACK processing"
	if (f ~ /^(tcp_v4_rcv|tcp_rcv_established|tcp_v4_do_rcv|tcp_data_queue|tcp_queue_rcv|tcp_event_data_recv|tcp_add_backlog|__release_sock|release_sock|ip_rcv|ip_local_deliver|ip_list_rcv|ip_sublist|__netif_receive|netif_receive|process_backlog|__napi_poll|napi_gro|gro_|dev_gro|skb_gro|inet_gro|tcp4_gro|tcp_gro|tcp_recvmsg|tcp_read_sock|tcp_read_done|tcp_cleanup_rbuf|sock_recvmsg|inet_recvmsg)/) return "TCP/IP receive"
	if (f ~ /^(tcp_sendmsg|tcp_write_xmit|tcp_push|__tcp_push|tcp_transmit_skb|__tcp_transmit|ip_queue_xmit|__ip_queue_xmit|ip_finish|ip_output|ip_local_out|__dev_queue_xmit|dev_queue_xmit|__dev_xmit_skb|sch_direct_xmit|dev_hard_start_xmit|qdisc_|fq_|tcp_send_ack|__tcp_send_ack|sk_reset_timer|tcp_small_queue|tcp_tsq|tcp_tasklet|sock_sendmsg|__sock_sendmsg|kernel_sendmsg)/) return "TCP/IP send"
	if (f ~ /^(xs_|xprt_|call_|rpc_|__rpc_|rpcauth|gss_|unx_|xdr_|rpcproc|svc_)/) return "sunrpc client"
	if (f ~ /^(nfs_|nfs3_|nfs4_|nfs_direct|nfs_file|nfs_pgio|nfs_pageio|nfs_read|nfs_initiate)/) return "NFS client"
	if (f ~ /^(io_|__io_|io_uring)/) return "io_uring"
	if (f ~ /^(iov_iter|__iov_iter|pin_user|gup_|__gup|get_user_pages|bio_|iomap_)/) return "iov and pages"
	if (f ~ /^(schedule|__schedule|try_to_wake_up|ttwu|finish_task_switch|pick_next|dequeue_task|enqueue_task|wake_up|__wake_up|select_task_rq|update_curr|psi_)/) return "scheduler, wakeups"
	if (f ~ /^(process_one_work|worker_thread|__queue_work|queue_work_on|kick_pool|insert_work)/) return "workqueue"
	if (f ~ /^(entry_SYSCALL|do_syscall_64|syscall_exit|__x64_sys|x64_sys_call|syscall_)/) return "syscall entry, exit"
	if (f ~ /^(handle_softirqs|__do_softirq|do_softirq|net_rx_action|net_tx_action|__local_bh_enable|irq_exit|__irq_exit)/) return "softirq (other)"
	if (f ~ /^(asm_common_interrupt|common_interrupt|asm_sysvec|sysvec_|__sysvec|handle_irq|__handle_irq|handle_edge|hrtimer_interrupt|__hrtimer_run|tick_|flush_smp_call|__flush_smp_call|generic_smp_call)/) return "IRQ, IPI, timer"
	return ""
}
function flush(   i, s, st, k) {
	if (!nf) return
	st = ""
	for (i = 1; i <= nf; i++) {
		if (fr[i] ~ /^(asm_common_interrupt|common_interrupt|asm_sysvec_.*|handle_softirqs|__do_softirq)$/) break
		if (fr[i] == "do_idle") { st = "idle"; break }
	}
	if (st == "")
		for (i = 1; i <= nf; i++) { s = stage(fr[i], ds[i]); if (s != "") { st = s; break } }
	if (st == "") st = "other"
	if (st != "idle") {
		g = grp(comm); n[st, g]++; tot[g]++; all++; stg[st] = 1; G[g] = 1
		leaf[fr[1], g]++; leaf[fr[1], "all"]++
		k = ""; for (i = nf; i >= 1; i--) k = k (k == "" ? "" : ";") fr[i]
		fold[comm ";" k]++
	}
	nf = 0
}
/^[^ \t]/ { flush(); m = match($0, /\[[0-9]+\]/); comm = substr($0, 1, RSTART - 1); sub(/[ \t]+$/, "", comm); sub(/^[ \t]+/, "", comm)
	sub(/-?[0-9]+$/, "", comm); sub(/[\/:].*$/, "", comm); gsub(/[ ;]/, "_", comm); next }
/^[ \t]+[0-9a-f]+ / { s = $2; sub(/\+0x[0-9a-f]+$/, "", s); ++nf; fr[nf] = s; ds[nf] = ($NF ~ /^\(\[/) ? "[k" : "user"; next }
/^$/ { flush() }
END {
	flush()
	u = 1e6 / hz / reads
	printf "READs in the window %d; us a READ from samples: all %.2f (fio %.2f, kworker %.2f, idle CPUs' interrupts %.2f, other %.2f)\n\n", reads, all * u, tot["fio"] * u, tot["kworker"] * u, tot["idle-irq"] * u, tot["other"] * u
	printf "%-28s %7s %7s %7s %7s %7s %6s\n", "stage", "fio", "kworker", "idleirq", "other", "all us", "share"
	for (s in stg) order[s] = n[s, "fio"] + n[s, "kworker"] + n[s, "idle-irq"] + n[s, "other"]
	PROCINFO["sorted_in"] = "@val_num_desc"
	for (s in order) printf "%-28s %7.2f %7.2f %7.2f %7.2f %7.2f %5.1f%%\n", s, n[s, "fio"] * u, n[s, "kworker"] * u, n[s, "idle-irq"] * u, n[s, "other"] * u, order[s] * u, 100 * order[s] / all
	delete order
	for (k in leaf) { split(k, a, SUBSEP); L[a[2]][a[1]] = leaf[k] }
	for (g in L) { f = out "/cleaf-" g ".txt"; c = 0
		for (s in L[g]) if (c++ < 60) printf "%7.3f us %5.2f%%  %s\n", L[g][s] * u, 100 * L[g][s] / (g == "all" ? all : tot[g]), s > f }
	PROCINFO["sorted_in"] = ""
	for (k in fold) print k, fold[k] > (out "/client.folded")
}
