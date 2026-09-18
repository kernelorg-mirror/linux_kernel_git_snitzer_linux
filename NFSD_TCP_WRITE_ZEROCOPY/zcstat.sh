#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# zcstat.sh -- vmstat-like periodic accounting for the NFSD TCP write-path
# page loan.  One row per interval answers the questions that decide whether
# the series is working on a given host and NIC:
#
#   1. Is the receive loan engaging?    pubrx vs cprx (loaned vs copy-mode
#      receives), brwMB vs cpdKB (bytes borrowed vs copied), pp% (how much
#      of the borrowed payload sits in page_pool pages -- ~100 on a NIC
#      that drives page_pool RX, 0 on loopback), mat (materialized), and
#      the named fallback reasons.
#   2. What payload geometry arrives?   avbv/mxbv = bvecs per published
#      receive of >= 512K.  Page-tiled 4K geometry gives ~257 per 1 MiB;
#      many hundreds means per-frame partial-page frags, i.e. a NIC
#      without header-data split (e.g. xeu).
#   3. Is the direct write path taken?  dirW/vecW counts and dirMB/vecMB
#      bytes (nfsd_write_direct / nfsd_write_vector), with the live
#      io_cache_write value in every row.
#
# nfsdC is the CPU consumed by the nfsd kernel threads in cores-equivalent
# (1.00 = one core saturated) -- the number the receive loan actually
# improves.  For the cleanest experiment, flip the loan kill-switch
# mid-run and watch pubrx/cprx and nfsdC respond at identical offered
# load:
#
#     echo N > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages
#     echo Y > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages
#
# drop counts trace-buffer overruns in the interval.  It must stay 0 for
# a row to be trustworthy: a non-zero drop means events were lost and
# every count in that row is an undercount.
#
# Every completed receive emits exactly one action=publish lifetime event
# (mode=copy on the copy path, mode=published on the loaned path), so rows
# count receives exactly once.  Uses a private tracefs instance; global
# tracing state is untouched.  Needs gawk.
#
# Usage: zcstat.sh [interval_seconds] [count]      (default: 5, forever)
# Env:   BUFKB=<per-cpu trace buffer KB, default 8192>
#        NOFILTER=1 to skip the in-kernel event filter
#
# Full documentation: zcstat.sh-usage.md (this directory).

set -u
INTERVAL=${1:-5}
COUNT=${2:-0}
BUFKB=${BUFKB:-8192}
NOFILTER=${NOFILTER:-0}

TRACE=/sys/kernel/tracing
command -v gawk >/dev/null || { echo "zcstat: gawk required" >&2; exit 1; }
mountpoint -q $TRACE || mount -t tracefs tracefs $TRACE 2>/dev/null
[ -d $TRACE/events ] || { echo "zcstat: no tracefs at $TRACE" >&2; exit 1; }

INST=$TRACE/instances/zcstat.$$
mkdir "$INST" 2>/dev/null || { echo "zcstat: cannot create tracefs instance" >&2; exit 1; }

STATE=$(mktemp /tmp/zcstat.state.XXXXXX)
SNAP=$STATE.snap
PREV=$STATE.prev
: > "$PREV"
READER_PID=

cleanup() {
	if [ -n "$READER_PID" ]; then
		kill "$READER_PID" 2>/dev/null
		wait "$READER_PID" 2>/dev/null
	fi
	# the reader can take a beat to release trace_pipe; EBUSY until then
	rmdir "$INST" 2>/dev/null || { sleep 0.5; rmdir "$INST" 2>/dev/null; }
	rm -f "$STATE" "$STATE.tmp" "$SNAP" "$PREV"
}
# cleanup on EXIT only; INT/TERM must *exit* (which fires the EXIT trap) --
# a handler that merely runs cleanup would leave the main loop running with
# the tracing already torn down.
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A bigger per-CPU buffer is the first defence against losing events on a
# busy server; drop accounting below reports whether it was enough.
echo "$BUFKB" > "$INST/buffer_size_kb" 2>/dev/null

enable_event() {
	local e=$INST/events/$1/enable
	if [ -e "$e" ]; then
		echo 1 > "$e"
	else
		echo "zcstat: WARNING: event $1 not present (module not loaded?)" >&2
	fi
}
enable_event sunrpc/svcsock_tcp_rx_lifetime
enable_event nfsd/nfsd_write_direct
enable_event nfsd/nfsd_write_vector

# Only action=publish is consumed below, but all six actions reach
# trace_pipe -- three per receive -- so filtering in the kernel cuts the
# text this script has to parse by about two thirds.  That matters on a
# fast server, where parsing cost and lost events both distort the
# measurement.
#
# The filter is numeric because tracefs gives userspace no way to resolve
# an enum symbolically: 0 = SVC_TCP_RX_CLASSIFY and 4 = SVC_TCP_RX_RELEASE
# in enum svc_tcp_rx_action, which is identical across the -3 and -5
# branches of this series (verified).  Should a future kernel reorder that
# enum the filter would quietly keep the wrong events, so it is applied
# best-effort and the first interval sanity-checks it (below).
FILTERED=0
if [ "$NOFILTER" != 1 ] &&
   [ -e "$INST/events/sunrpc/svcsock_tcp_rx_lifetime/filter" ]; then
	if echo 'action != 0 && action != 4' \
	     > "$INST/events/sunrpc/svcsock_tcp_rx_lifetime/filter" 2>/dev/null; then
		FILTERED=1
	else
		echo "zcstat: note: could not set event filter; continuing unfiltered" >&2
	fi
fi

# Reader: aggregate cumulative counters, flush atomically to $STATE at most
# once a second.  If no events arrive the state file simply stays put and
# the main loop prints a zero-delta row -- exactly right.
gawk -v STATE="$STATE" '
function dump(   r, t) {
	t = STATE ".tmp"
	printf "pub %d\ncp %d\nbrw %d\ncpd %d\nmat %d\npp %d\nlgrx %d\nlgbv %d\nmxbv %d\ndir %d\nvec %d\ndirb %d\nvecb %d\nlifelines %d\n", \
		pub, cp, brw, cpd, mat, pp, lgrx, lgbv, mxbv, dir, vec, dirb, vecb, lifelines > t
	for (r in reas)
		printf "reason.%s %d\n", r, reas[r] > t
	close(t)
	system("mv " t " " STATE)
	last = systime()
}
{
	if (index($0, "nfsd_write_direct:")) {
		dir++
		if (match($0, /len=[0-9]+/))
			dirb += substr($0, RSTART + 4, RLENGTH - 4) + 0
	} else if (index($0, "nfsd_write_vector:")) {
		vec++
		if (match($0, /len=[0-9]+/))
			vecb += substr($0, RSTART + 4, RLENGTH - 4) + 0
	} else if (index($0, "svcsock_tcp_rx_lifetime:")) {
		# counted only for lifetime events, so the periodic
		# trace_marker tick and the nfsd write events cannot make an
		# idle interval look like a misparse
		lifelines++
		act = ""; mode = ""; reason = ""; body = 0
		borrowed = 0; copied = 0; matz = 0; bv = 0; ppb = 0
		n = split($0, f, " ")
		for (i = 1; i <= n; i++) {
			p = index(f[i], "="); if (p < 2) continue
			k = substr(f[i], 1, p - 1); v = substr(f[i], p + 1)
			if (k == "action") act = v
			else if (k == "mode") mode = v
			else if (k == "reason") reason = v
			else if (k == "body") body = v + 0
			else if (k == "borrowed") borrowed = v + 0
			else if (k == "copied") copied = v + 0
			else if (k == "materialized") matz = v + 0
			else if (k == "page_pool") ppb = v + 0
			else if (k == "bvecs") bv = v + 0
		}
		if (act == "publish") {
			if (mode == "copy") cp++; else pub++
			brw += borrowed; cpd += copied; mat += matz; pp += ppb
			if (reason != "none" && reason != "") reas[reason]++
			if (mode != "copy" && body >= 524288) {
				lgrx++; lgbv += bv
				if (bv > mxbv) mxbv = bv
			}
		}
	}
	if (systime() > last) dump()
}
END { dump() }
' < "$INST/trace_pipe" &
READER_PID=$!

CLK=$(getconf CLK_TCK)

nfsd_ticks() {
	local t=0 p v
	for p in $(pgrep -x nfsd 2>/dev/null); do
		v=$(gawk '{print $14 + $15}' /proc/$p/stat 2>/dev/null) || continue
		t=$((t + ${v:-0}))
	done
	echo $t
}

nfsd_threads() { pgrep -xc nfsd 2>/dev/null || echo 0; }

# Sum of per-CPU ring overruns: events the kernel dropped because the
# buffer was full.
trace_overrun() {
	gawk '/^overrun:/ { t += $2 } END { print t + 0 }' \
		"$INST"/per_cpu/cpu*/stats 2>/dev/null || echo 0
}

uptime_s() { gawk '{print $1}' /proc/uptime; }

knob() { cat "$1" 2>/dev/null || echo '-'; }

header() {
	printf '%2s %4s | %6s %6s %8s %7s %4s %5s | %5s %5s %5s | %6s %8s %6s %8s | %6s %5s  %s\n' \
		sw iocw pubrx cprx brwMB cpdKB 'pp%' mat lgrx avbv mxbv \
		dirW dirMB vecW vecMB nfsdC drop 'fallback-reasons'
}

rows=0
prev_ticks=$(nfsd_ticks)
prev_over=$(trace_overrun)
prev_up=$(uptime_s)
echo "zcstat: interval ${INTERVAL}s, buffer ${BUFKB}KB/cpu, event filter $([ "$FILTERED" = 1 ] && echo on || echo off), nfsd threads $(nfsd_threads)"
header
while :; do
	sleep "$INTERVAL"

	# Wake the reader: it flushes at most once a second, but only when a
	# line arrives -- a burst that goes quiet would otherwise sit
	# unflushed until the next event.  A marker write is that line.
	echo zcstat-tick > "$INST/trace_marker" 2>/dev/null
	sleep 0.3

	ticks=$(nfsd_ticks)
	dt_ticks=$((ticks - prev_ticks))
	prev_ticks=$ticks
	over=$(trace_overrun)
	d_over=$((over - prev_over))
	prev_over=$over
	# real elapsed time, not the nominal interval: the marker sleep and
	# this bookkeeping would otherwise inflate every rate by ~6%.
	up=$(uptime_s)
	elapsed=$(gawk -v a="$prev_up" -v b="$up" 'BEGIN{d=b-a; print (d>0)?d:1}')
	prev_up=$up
	sw=$(knob /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages)
	iocw=$(knob /sys/kernel/debug/nfsd/io_cache_write)

	cp -f "$STATE" "$SNAP" 2>/dev/null || : > "$SNAP"
	gawk -v dtt="$dt_ticks" -v clk="$CLK" -v dt="$elapsed" \
	     -v sw="$sw" -v iocw="$iocw" -v dover="$d_over" \
	     -v filtered="$FILTERED" -v prevf="$PREV" '
	FILENAME == prevf { a[$1] = $2; next }
	{ b[$1] = $2 }
	END {
		pub  = b["pub"]  - a["pub"];   cp2  = b["cp"]   - a["cp"]
		brw  = b["brw"]  - a["brw"];   cpd  = b["cpd"]  - a["cpd"]
		mat  = b["mat"]  - a["mat"];   pp   = b["pp"]   - a["pp"]
		lgrx = b["lgrx"] - a["lgrx"];  lgbv = b["lgbv"] - a["lgbv"]
		dir  = b["dir"]  - a["dir"];   vec  = b["vec"]  - a["vec"]
		dirb = b["dirb"] - a["dirb"];  vecb = b["vecb"] - a["vecb"]
		lifelines = b["lifelines"] - a["lifelines"]
		avbv = lgrx > 0 ? lgbv / lgrx : 0
		ppct = brw > 0 ? pp * 100.0 / brw : 0
		rs = ""
		for (k in b) {
			if (substr(k, 1, 7) != "reason.") continue
			d = b[k] - a[k]
			if (d > 0)
				rs = rs (rs == "" ? "" : ",") substr(k, 8) "=" d
		}
		if (dover > 0)
			rs = rs (rs == "" ? "" : " ") "[EVENTS LOST - counts are low]"
		# A wrong numeric action filter looks exactly like this: trace
		# lines arriving, none of them a publish.
		if (filtered == 1 && lifelines > 0 && pub == 0 && cp2 == 0)
			rs = rs (rs == "" ? "" : " ") "[no publish events: rerun with NOFILTER=1]"
		printf "%2s %4s | %6d %6d %8.1f %7.1f %4.0f %5d | %5d %5.0f %5d | %6d %8.1f %6d %8.1f | %6.2f %5d  %s\n", \
			sw, iocw, pub, cp2, brw / 1048576, cpd / 1024, ppct, mat, \
			lgrx, avbv, b["mxbv"] + 0, \
			dir, dirb / 1048576, vec, vecb / 1048576, \
			dtt / (clk * dt), dover, rs
	}' "$PREV" "$SNAP"

	cp -f "$SNAP" "$PREV"
	rows=$((rows + 1))
	[ "$COUNT" -gt 0 ] && [ "$rows" -ge "$COUNT" ] && break
	[ $((rows % 20)) -eq 0 ] && header
done
