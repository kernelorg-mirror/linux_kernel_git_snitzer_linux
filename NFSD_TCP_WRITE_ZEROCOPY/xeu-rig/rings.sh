#!/bin/bash
# rings.sh [seconds] - what each RX ring of the port took over a few seconds
# of whatever load is running: GB/s, its share of the port, frames, and the
# tail drops over the same time.  Read it during a load, not after: ring
# counters are zeroed by a ring change or a down/up.
. "$(dirname "$0")/common.sh"
N=${1:-5}
snap() { ethtool -S $D | awk -F': *' '$1 ~ /ring_[0-9]+_rx_(bytes|packets)$/ {gsub(/ /, "", $1); print $1, $2}'; }
a=$(snap); t0=$(tail_drops); sleep $N; b=$(snap); t1=$(tail_drops)
paste <(echo "$a") <(echo "$b") | awk -v n=$N '
	{ split($1, f, "_"); r = f[2]; d = $4 - $2
	  if ($1 ~ /bytes$/) { by[r] = d; tot += d } else pk[r] = d
	  if (!(r in seen)) { seen[r] = 1; order[++k] = r } }
	END { printf "%-6s %8s %7s %12s\n", "ring", "GB/s", "share", "frames"
	      for (i = 1; i <= k; i++) { r = order[i]
		printf "%-6s %8.2f %6.1f%% %12d\n", r, by[r] / n / 1e9, tot ? 100 * by[r] / tot : 0, pk[r] }
	      printf "%-6s %8.2f\n", "all", tot / n / 1e9 }'
echo "tail drops in $N s: $(( t1 - t0 ))"
