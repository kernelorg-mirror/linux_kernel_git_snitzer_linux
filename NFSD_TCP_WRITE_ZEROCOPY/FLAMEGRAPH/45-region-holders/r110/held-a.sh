#!/bin/bash
# held-a.sh PAGES FILE: run110's held table for port a alone, port b's 32768
# posted buffers (eight idle rings of 4096, outside the region) taken off
awk -v pages=$1 '{for (i = 1; i <= NF; i++) {split($i, kv, "="); v[kv[1]] = kv[2]}
	u = pages - v["free"]; ring = (v["posted"] - 32768) / 2; pool = v["cache"] + v["stack"]; q = (v["rmem"] + v["bl"]) / 16384
	ch = v["budget"] ? v["budget"] - v["avail"] - v["reserve"] : 0; rest = u - ring - pool - q - ch
	n++; su += u; sr += ring; sp += pool; sc += v["cache"]; ss += v["stack"]; sq += q; sch += ch; srest += rest; sinf += v["inflight"]
	if (u > mu) {mu = u; mr = ring; mp = pool; mc = v["cache"]; mk = v["stack"]; mq = q; mch = ch; mrest = rest; minf = v["inflight"]}}
	END {printf "mean %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.1f\nmost %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f\n", su/n, sr/n, sp/n, sc/n, ss/n, sq/n, sch/n, srest/n, sinf/n, mu, mr, mp, mc, mk, mq, mch, mrest, minf}' $2
