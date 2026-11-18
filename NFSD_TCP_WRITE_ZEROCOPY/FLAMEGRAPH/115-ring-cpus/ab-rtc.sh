#!/bin/bash
# ab-rtc.sh <n>:<cpus> ...: FLAMEGRAPH round 115, the rtc work items on
# the ring CPUs too, for 4 KiB READ IOPS (Mike, 18 November: "run the ring
# CPUs A/B with fh_borrow_slots 48").  For each run: one settled-iops.sh
# run of nfs4k.sh <n> (iodepth 64, nconnect 12, all three borrows held,
# fh_borrow_slots 48, the client at scope cpu) with EXP_RTC_CPUS=<cpus>
# (sunrpc's svc_rtc_cpus; "settled" for the settled 8-55), then
# settled.sh apply without it, since settled-iops.sh's own apply after a
# run drops only the variables it sets.  Printed beside each run: the
# work CPUs' (8 to 55) and the ring CPUs' (0 to 7, 56 to 63) mean and
# highest busy from the run's mpstat, and the borrow counters' change.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
B=/sys/kernel/debug/nfsd/export_borrow
L=${L:-/var/tmp/zc-run-logs/r115}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
delta() {
	awk -v a="$1" -v b="$2" 'BEGIN {
		n = split(a, x, " "); split(b, y, " ")
		for (i = 1; i < n; i += 2) printf "%s %d ", x[i], y[i + 1] - x[i + 1]
		print "" }'
}
cpus_busy() {	# cpus_busy <mpstat file>: work and ring CPUs' mean and max busy
	awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {
		b = 100 - $NF; c = $2 + 0
		if (c >= 8 && c <= 55) { w += b; nw++; if (b > mw) mw = b }
		else { r += b; nr++; if (b > mr) mr = b } }
	END { printf "work CPUs %.1f%% (max %.0f%%), ring CPUs %.1f%% (max %.0f%%)", w / nw, mw, r / nr, mr }' "$1"
}
for run in "$@"; do
	IFS=: read n cpus <<< "$run"
	[ -n "$n" ] && [ -n "$cpus" ] || { say "ab-rtc.sh <n>:<cpus> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	b0=$(cat $B)
	if [ "$cpus" = settled ]; then
		./step.sh $L rtc-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	else
		EXP_RTC_CPUS=$cpus ./step.sh $L rtc-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1
	fi
	rc=$?
	b1=$(cat $B)
	./settled.sh apply > $L/settled-after-$n.txt 2>&1
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n svc_rtc_cpus $cpus rc $rc | $(cpus_busy /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt) | rtc_cpus in run: $(grep -oE 'svc_rtc_cpus=[0-9,-]+' $L/t2-r$n.log | head -1) | after: $(tail -1 $L/settled-after-$n.txt)"
	say "t2-r$n borrow: $(delta "$b0" "$b1")"
done
