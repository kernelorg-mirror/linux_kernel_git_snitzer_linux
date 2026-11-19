# conn.awk conn-bpf.txt: one line a connection from conn.bt's maps over its
# 2 s, then a summary: which connections took their calls through the rtc
# work item, which through nfsd threads, and where their work CPUs are.
/^@[a-z_]+\[/ {
	m = $1; sub(/^@/, "", m); k = m; sub(/\[.*/, "", m); sub(/^[a-z_]+\[/, "", k); sub(/\]:$/, "", k)
	v = $2; x[k] = 1; d[m, k] = v
}
END {
	printf "%-14s %-6s %-4s %-4s %9s %9s %s\n", "server", "client", "cpu", "ran", "rtc/s", "thread/s", "thread_calls"
	for (k in x) {
		ip = d["lip", k]; if (ip == "") ip = "?"
		if (ip !~ /^192\.168\./) continue
		r = d["rtc", k] / 2; t = d["thr", k] / 2; c = (("cpu", k) in d) ? d["cpu", k] : -1
		printf "%-14s %-6s %-4s %-4s %9d %9d %s\n", ip, d["rport", k], c, (("ran_on", k) in d) ? d["ran_on", k] : "-", r, t, d["tc", k] | "sort -k1,1 -k3,3n"
		n++; if (r > t) nr++; else { nt++; tl = tl " " ip ":" d["rport", k] "(cpu " c ")" }
		if (c >= 0 && (c < 8 || c > 55)) { no++; nl = nl " " ip ":" d["rport", k] "(cpu " c ")" }
		rs += r; ts += t
	}
	close("sort -k1,1 -k3,3n")
	printf "connections %d: %d through the rtc work item, %d through threads%s\n", n, nr, nt, tl
	printf "work CPUs off nfsd's 8-55: %d%s\n", no, nl
	printf "calls a second: rtc %d, threads %d\n", rs, ts
}
