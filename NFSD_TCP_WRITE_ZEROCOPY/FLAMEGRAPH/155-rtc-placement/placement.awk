# placement.awk rtc-placed.txt srv-mpstat.txt: each second's connections
# a CPU over nfsd's CPUs 8-55, then each CPU's mean connections over the
# load's seconds 8 to 18 (mpstat's window) beside its busy.
FNR == 1 { f++ }
f == 1 && $1 == "t" { s = $3; ns++; sec[ns] = s; next }
f == 1 && $1 == "rebalanced" { reb[ns] = $2; next }
f == 1 && $1 == "placed" {
	delete c; tot = 0
	for (i = 2; i <= NF; i++) { split($i, a, ":"); c[a[1]] = a[2] }
	h0 = h1 = h2 = h3 = h4 = 0; mn = 99; mx = 0; off = 0
	for (cpu = 8; cpu <= 55; cpu++) {
		k = c[cpu] + 0; tot += k
		if (k < mn) mn = k; if (k > mx) mx = k
		if (k == 0) h0++; else if (k == 1) h1++; else if (k == 2) h2++; else if (k == 3) h3++; else h4++
		if (sec[ns] >= 8 && sec[ns] <= 18) { sum[cpu] += k; nw[cpu]++ }
	}
	for (cpu in c) if (cpu < 8 || cpu > 55) off += c[cpu]
	line[ns] = sprintf("s%-3d placed %3d (+%d off 8-55), a CPU %d to %d; CPUs with 0:%d 1:%d 2:%d 3:%d 4+:%d", sec[ns], tot, off, mn, mx, h0, h1, h2, h3, h4)
	next
}
f == 2 && $1 != "Average:" && $2 ~ /^[0-9]+$/ && $2 >= 8 && $2 <= 55 { busy[$2] = 100 - $NF }
END {
	for (i = 1; i <= ns; i++) if (i in line) printf "%s, rebalanced %d\n", line[i], reb[i] - reb[1]
	for (cpu = 8; cpu <= 55; cpu++) if (nw[cpu]) {
		m = sum[cpu] / nw[cpu]; b = int(m + 0.5); if (b > 4) b = 4
		bs[b] += busy[cpu]; bn[b]++; bl[b] = bl[b] " " cpu
	}
	for (b = 0; b <= 4; b++) if (bn[b]) printf "seconds 8-18, about %d%s connections: %d CPUs, busy %.0f%% on average:%s\n", b, b == 4 ? "+" : "", bn[b], bs[b] / bn[b], bl[b]
}
