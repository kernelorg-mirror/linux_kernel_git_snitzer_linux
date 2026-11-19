# spread.awk: FLAMEGRAPH round 147.  Reads perf script -F cpu,comm,sym
# of the server (comm, [cpu], sym); with -v M=<srv-mpstat-w.txt> each
# CPU's busy from the run, and -v S=<file> for each CPU's symbols of 2%
# or more ("cpu share sym", sorted after).  Kinds: an rtc work
# item is a kworker in svc_rtc_* or under it (by its comm, a kworker not
# nfsd), nfsd a thread named nfsd, softirq any sample in a softirq-only
# symbol family, idle the idle loop.
function kind(comm, sym) {
	if (sym ~ /^(cpu_idle_poll|default_idle_call|cpuidle_|arch_cpu_idle|do_idle)/ || comm == "swapper") return "idle"
	if (comm ~ /^nfsd/) return "nfsd"
	if (comm ~ /^ksoftirqd/ || sym ~ /^(net_rx_action|__napi_poll|napi_|xeu_|tcp_v4_rcv|ip_rcv|__netif_receive|handle_softirqs)/) return "softirq"
	if (comm ~ /^kworker/) return "kworker"
	return "other"
}
BEGIN {
	if (M != "")
		while ((getline l < M) > 0) {
			split(l, f, " ")
			if (f[2] ~ /^[0-9]+$/ && f[1] != "Average:") { bn[f[2]]++; bu[f[2]] += 100 - f[length(f)] }
		}
}
{
	comm = $1; c = $2; gsub(/[\[\]]/, "", c); c += 0
	sym = NF >= 3 ? $NF : ""; sub(/\+0x.*/, "", sym)
	k = kind(comm, sym); K[c, k]++; T[c]++; Y[c, sym]++; ks[k] = 1
}
END {
	printf "%4s %-6s %5s %6s %6s %8s %8s %6s\n", "CPU", "role", "busy", "rtc/kw", "nfsd", "softirq", "other", "idle"
	for (c = 0; c < 64; c++) {
		if (!T[c]) continue
		r = (c < 8 || c > 55) ? "rx" : "work"
		printf "%4d %-6s %4.0f%% %5.0f%% %5.0f%% %7.0f%% %7.0f%% %5.0f%%\n", c, r, bn[c] ? bu[c] / bn[c] : -1, 100 * K[c, "kworker"] / T[c], 100 * K[c, "nfsd"] / T[c], 100 * K[c, "softirq"] / T[c], 100 * K[c, "other"] / T[c], 100 * K[c, "idle"] / T[c]
	}
	if (S != "")
		for (key in Y) {
			split(key, p, SUBSEP)
			if (Y[key] * 50 >= T[p[1]])
				printf "%d %.1f%% %s\n", p[1], 100 * Y[key] / T[p[1]], p[2] > S
		}
}
