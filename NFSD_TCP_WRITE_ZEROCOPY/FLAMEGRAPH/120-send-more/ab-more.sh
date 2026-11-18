#!/bin/bash
# ab-more.sh <n>:<Y|N> ...: FLAMEGRAPH round 120, the rtc send FIFO's
# replies batched with MSG_MORE (Mike, 18 November: "implement reply send
# batching, test and then show me the updated Per-READ breakdown").  For
# each run: one settled-iops.sh run of nfs4k.sh <n> with
# EXP_RTC_SEND_MORE=<Y|N>, then settled.sh apply.  Printed beside each:
# the work and ring CPUs' busy, and from rtc_stats and nstat over the
# run, the replies sent, those with MSG_MORE, and TCP's segments out and
# in a reply.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r120}
S=/sys/kernel/debug/nfsd/rtc_stats
mkdir -p $L
say() { echo "$(date +%T) $*"; }
stat1() { awk -v k="$1" '$1 == k { print $2 }' $S; }
nst() { nstat -az "$1" | awk -v k="$1" '$1 == k { print $2 }'; }
cpus_busy() {
	awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {
		b = 100 - $NF; c = $2 + 0
		if (c >= 8 && c <= 55) { w += b; nw++; if (b > mw) mw = b }
		else { r += b; nr++; if (b > mr) mr = b } }
	END { printf "work CPUs %.1f%% (max %.0f%%), ring CPUs %.1f%% (max %.0f%%)", w / nw, mw, r / nr, mr }' "$1"
}
for run in "$@"; do
	IFS=: read n more <<< "$run"
	case $more in Y|N) ;; *) say "ab-more.sh <n>:<Y|N> ..."; exit 1 ;; esac
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	s0=$(stat1 sends) m0=$(stat1 send_more) o0=$(nst TcpOutSegs) i0=$(nst TcpInSegs)
	EXP_RTC_SEND_MORE=$more ./step.sh $L more-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	rc=$?
	s1=$(stat1 sends) m1=$(stat1 send_more) o1=$(nst TcpOutSegs) i1=$(nst TcpInSegs)
	./settled.sh apply > $L/settled-after-$n.txt 2>&1
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n send_more $more rc $rc | $(cpus_busy /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt) | $(awk -v s=$((s1 - s0)) -v m=$((m1 - m0)) -v o=$((o1 - o0)) -v i=$((i1 - i0)) 'BEGIN { printf "replies %d, with MSG_MORE %.1f%%, segments out %.3f and in %.3f a reply", s, s ? 100 * m / s : 0, s ? o / s : 0, s ? i / s : 0 }') | in run: $(grep -oE 'svc_rtc_send_more=[YN]' $L/t2-r$n.log | head -1) | after: $(tail -1 $L/settled-after-$n.txt)"
done
