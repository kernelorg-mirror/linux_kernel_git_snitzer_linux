#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run-rxpasses.sh -- measure receive-pass amplification in the SUNRPC TCP
# receive path, and what it costs in nfsd wakeups.
#
# Why this exists.  On the loopback rig the page loan makes each receive
# pass ~4x cheaper and cuts total time in svc_tcp_recvfrom() by ~15%, but
# it needs ~2x as many passes per record (10.6 vs 5.2 for a 1 MiB record)
# and so draws ~4 more nfsd wakeups per MiB.  The pass is cheap enough
# that the socket queue drains, the thread sleeps, and sk_data_ready wakes
# it again.  That second-order cost is the one part of the loopback
# profile that should transfer to real hardware -- at MTU 1500 a 1 MiB
# record arrives as hundreds of skbs rather than ~17, so the pass
# arithmetic may look very different.  Measure it before optimising it.
#
# Purely observational: it does not generate load.  Drive the workload
# from the real client (1 MiB writes), then run this on the server.  It
# derives everything from what the server sees, so it needs no knowledge
# of the client's workload:
#
#   - passes   = calls to svc_tcp_recvfrom()
#   - records  = those calls that returned > 0, i.e. a complete RPC record
#   - so passes/record needs no workload bookkeeping at all
#
# Usage:
#   run-rxpasses.sh [seconds]          observe the current loan setting
#   run-rxpasses.sh [seconds] --ab     observe loans Y, then N, then restore
#
# Needs bpftrace and a kernel carrying the page-loan series.  Safe on a
# production server: kprobes plus one sched tracepoint, no load, no knob
# changes unless --ab is given (which restores what it found).

set -u
SECS=${1:-30}
MODE=${2:-}
SW=/sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages

command -v bpftrace >/dev/null || { echo "run-rxpasses: bpftrace required" >&2; exit 1; }
[ -r /proc/net/rpc/nfsd ] || { echo "run-rxpasses: nfsd not running" >&2; exit 1; }
grep -q svc_tcp_recvfrom /proc/kallsyms || {
	echo "run-rxpasses: svc_tcp_recvfrom not found -- is sunrpc loaded?" >&2; exit 1; }

BT=$(mktemp /tmp/rxpasses.XXXXXX.bt)
OUT=$(mktemp /tmp/rxpasses.XXXXXX.out)
SUM=$(mktemp /tmp/rxpasses.XXXXXX.sum)
trap 'rm -f "$BT" "$OUT" "$SUM"' EXIT

cat > "$BT" <<'EOF'
kprobe:svc_tcp_recvfrom { @t[tid] = nsecs; }

kretprobe:svc_tcp_recvfrom /@t[tid]/ {
	$d = nsecs - @t[tid];
	delete(@t[tid]);
	@passes = count();
	@pass_ns = sum($d);
	@pass_us = hist($d / 1000);
	if (retval > 0) {
		/* a complete record: the return value is its length */
		@records = count();
		@record_kb = hist(retval / 1024);
		@record_bytes = sum(retval);
		@full_pass_us = hist($d / 1000);
	} else {
		@empty_pass_us = hist($d / 1000);
	}
}

/* only wakeups targeting an nfsd thread */
tracepoint:sched:sched_wakeup /args->comm == "nfsd"/ {
	@nfsd_wakeups = count();
}
EOF

sample() {   # $1 = label
	local label=$1 io0 io1 ct0 ct1 wr

	bpftrace "$BT" > "$OUT" 2>/dev/null &
	local bp=$!
	# let the probes attach first: the byte counters must be read inside
	# the probe window, or MiB/s and every per-MiB ratio are skewed by
	# the attach delay
	sleep 3
	io0=$(awk '/^io/ {print $3}' /proc/net/rpc/nfsd)
	ct0=$(awk '/^ctxt/ {print $2}' /proc/stat)

	sleep "$SECS"

	io1=$(awk '/^io/ {print $3}' /proc/net/rpc/nfsd)
	ct1=$(awk '/^ctxt/ {print $2}' /proc/stat)
	kill -INT $bp 2>/dev/null
	wait $bp 2>/dev/null
	wr=$((io1 - io0))

	local passes records pass_ns wakeups rbytes
	passes=$(awk '/^@passes:/  {print $2}' "$OUT"); passes=${passes:-0}
	records=$(awk '/^@records:/ {print $2}' "$OUT"); records=${records:-0}
	pass_ns=$(awk '/^@pass_ns:/ {print $2}' "$OUT"); pass_ns=${pass_ns:-0}
	wakeups=$(awk '/^@nfsd_wakeups:/ {print $2}' "$OUT"); wakeups=${wakeups:-0}
	rbytes=$(awk '/^@record_bytes:/ {print $2}' "$OUT"); rbytes=${rbytes:-0}

	echo "== $label  (loan switch: $(cat $SW 2>/dev/null || echo '?'), ${SECS}s window)"
	if [ "$passes" -eq 0 ]; then
		echo "   no receive activity -- is the client driving writes at the server?"
		echo
		return
	fi
	awk -v p="$passes" -v r="$records" -v ns="$pass_ns" -v wk="$wakeups" \
	    -v wr="$wr" -v rb="$rbytes" -v secs="$SECS" -v sf="$SUM" -v lb="$label" 'BEGIN {
		if (sf != "")
			printf "%s|%.0f|%.2f|%.2f|%.2f\n", lb, (wr/1048576.0)/secs,
			       (r>0)? p/r : 0, (ns/1000.0)/p,
			       (wr>0)? wk/(wr/1048576.0) : 0 >> sf
		mib = wr / 1048576.0
		printf "   passes            %10d\n", p
		printf "   records           %10d", r
		if (r > 0) printf "   (mean %.1f KiB each)", (rb/r)/1024.0
		printf "\n"
		if (r > 0)
		  printf "   passes/record     %10.2f   <- the amplification\n", p/r
		printf "   us/pass (mean)    %10.2f\n", (ns/1000.0)/p
		printf "   recvfrom ms total %10.1f   (%.1f%% of one core)\n", \
			ns/1e6, (ns/1e7)/secs
		printf "   nfsd wakeups      %10d", wk
		if (mib > 0) printf "   = %.2f per MiB written", wk/mib
		printf "\n"
		if (mib > 0) {
		  printf "   NFSD MiB written  %10.1f   (%.0f MiB/s)\n", mib, mib/secs
		  printf "   passes/MiB        %10.2f\n", p/mib
		}
	}'
	echo "   --- per-pass duration (us), all passes:"
	sed -n '/^@pass_us:/,/^$/p' "$OUT" | sed '1d;/^$/d' | sed 's/^/   /'
	echo "   --- passes that completed a record:"
	sed -n '/^@full_pass_us:/,/^$/p' "$OUT" | sed '1d;/^$/d' | sed 's/^/   /'
	echo
}

if [ "$MODE" = "--ab" ]; then
	[ -w "$SW" ] || { echo "run-rxpasses: cannot write $SW for --ab" >&2; exit 1; }
	ORIG=$(cat "$SW")
	restore() { echo "$ORIG" > "$SW" 2>/dev/null; rm -f "$BT" "$OUT" "$SUM"; }
	trap restore EXIT
	echo "run-rxpasses: A/B over $((SECS * 2 + 6))s plus settle; keep offered load constant"
	echo
	echo Y > "$SW"; sleep 2
	sample "loans ON"
	echo N > "$SW"; sleep 2
	sample "loans OFF (pre-loan receive)"
	echo "$ORIG" > "$SW"
	echo "loan switch restored to $ORIG"
	echo
	awk -F'|' 'BEGIN {
		printf "%-26s %9s %14s %10s %14s\n", "window", "MiB/s", "passes/record", "us/pass", "wakeups/MiB"
	}
	{ printf "%-26s %9s %14s %10s %14s\n", $1, $2, $3, $4, $5
	  n++; mibps[n] = $2 + 0; ppr[n] = $3 + 0 }
	END {
		print ""
		if (n < 2) { print "only one window captured -- nothing to compare"; exit }
		hi = (mibps[1] > mibps[2]) ? mibps[1] : mibps[2]
		lo = (mibps[1] < mibps[2]) ? mibps[1] : mibps[2]
		if (hi > 0 && (hi - lo) / hi > 0.25) {
			print "WARNING: the two windows saw very different throughput"
			printf "         (%.0f vs %.0f MiB/s). Offered load was not constant, so\n", mibps[1], mibps[2]
			print "         absolute rates are NOT comparable across the windows."
			print "         passes/record and us/pass are per-event and still valid."
		} else {
			print "Offered load was comparable across windows; all columns comparable."
		}
		print ""
		if (ppr[1] > 0 && ppr[2] > 0)
			printf "Pass amplification with loans: %.2fx (%.2f vs %.2f passes/record)\n", \
				ppr[1]/ppr[2], ppr[1], ppr[2]
	}' "$SUM"
	echo
	echo "If passes/record is far above the ~10 seen on a 4K loopback rig, the"
	echo "amplification is worth attacking (a bounded re-poll before the thread"
	echo "sleeps) -- capture this output on the bug."
else
	sample "current setting"
fi
