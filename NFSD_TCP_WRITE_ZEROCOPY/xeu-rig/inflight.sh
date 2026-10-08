#!/bin/bash
# inflight.sh <pid> <out> - while <pid> lives, record the most loans in
# flight and the most waiters seen at each budget (four samples a second).
# A budget "of n WRITEs" should show max_inflight n.
while kill -0 $1 2>/dev/null; do sysctl -n net.sunrpc.region_budget_stats; sleep 0.25; done |
awk -v o="$2" '{ b = $2; i = $10; w = $12
	if (b > 0 && (i > mi[b] || w > mw[b])) {
		if (i > mi[b]) mi[b] = i; if (w > mw[b]) mw[b] = w
		printf "%s budget %s max_inflight %s max_waiters %s\n", strftime("%T"), b, mi[b], mw[b] >> o; fflush(o) } }'
