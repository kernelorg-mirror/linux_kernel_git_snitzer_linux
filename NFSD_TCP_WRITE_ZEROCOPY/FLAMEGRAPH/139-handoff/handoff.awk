# handoff.awk: FLAMEGRAPH round 139.  Reads perf script -F
# cpu,time,event,trace of the five sunrpc tracepoints; follows each task
# (the task:<task>@<client> of its records; xprt_lookup_rqst has the
# xid only, unique enough across 24 transports in 0.3 s) from begin through its xid to the completion, and
# for each pair of stages counts how often they ran on the same CPU, on
# another CPU of the same socket (CPUs 0-23 and 24-47 here, one L3
# each) or on the other socket, with the time between them.
function sock(c) { return c < 24 ? 0 : 1 }
function kv(k,   i) { for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) return substr($i, length(k) + 2); return "" }
function rel(a, b) { return a == b ? "same CPU" : sock(a) == sock(b) ? "same socket" : "other socket" }
function pair(name, ca, ta, cb, tb,   r) {
	r = rel(ca, cb); P[name, r]++; N[name]++; T[name, r] += tb - ta; names[name] = 1
}
{
	cpu = $1; gsub(/[\[\]]/, "", cpu); cpu += 0
	t = $2; sub(/:$/, "", t); t += 0
	ev = $3; sub(/^sunrpc:/, "", ev); sub(/:$/, "", ev)
	task = ""; for (i = 4; i <= NF; i++) if (index($i, "task:") == 1) { task = substr($i, 6); break }
	x = kv("xid")
}
ev == "rpc_task_begin" && task != "" { bc[task] = cpu; bt[task] = t; next }
ev == "xprt_transmit" { if (task != "") { tc[task] = cpu; tt[task] = t; if (x != "") xt[x] = task }; next }
ev == "xprt_lookup_rqst" { if (x != "" && (x in xt)) { k = xt[x]; lc[k] = cpu; lt[k] = t }; next }
ev == "rpc_stats_latency" && task != "" {
	if (task in bc) pair("submit -> send", bc[task], bt[task], (task in tc) ? tc[task] : cpu, (task in tt) ? tt[task] : t)
	if ((task in lc) && (task in tc)) pair("send -> reply matched", tc[task], tt[task], lc[task], lt[task])
	if (task in lc) pair("reply matched -> completion", lc[task], lt[task], cpu, t)
	if (task in bc) pair("submit -> completion", bc[task], bt[task], cpu, t)
	done++; next
}
END {
	printf "events read: %d tasks followed to their completion\n", done
	for (s in names) {
		printf "%s (%d)\n", s, N[s]
		split("same CPU,same socket,other socket", R, ",")
		for (i = 1; i <= 3; i++) if (P[s, R[i]]) printf "  %-13s %5.1f%%  mean %7.1f us\n", R[i], 100 * P[s, R[i]] / N[s], 1e6 * T[s, R[i]] / P[s, R[i]]
	}
}
