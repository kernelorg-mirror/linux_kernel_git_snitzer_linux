#!/bin/bash
# ab-spill.sh <n>:<off|w<k>> ...: FLAMEGRAPH round 116, the rtc balance
# spilling onto the ring CPUs as a last resort (Mike, 18 November:
# "implement the spill weight option and A/B it").  For each run: one
# settled-iops.sh run of nfs4k.sh <n> (iodepth 64, nconnect 12, the three
# borrows held, the client at scope cpu); with w<k>, svc_rtc_spill_cpus
# 0-7,56-63 (the ring CPUs) at svc_rtc_spill_weight <k> for the run, then
# settled.sh apply without them.  At the load's second 10, rtc_stats'
# "placed" line (connections a CPU) is saved, and the connections on ring
# CPUs counted.  Printed beside each run: the work CPUs' (8 to 55) and the
# ring CPUs' mean and highest busy from its mpstat.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r116}
RINGS=0-7,56-63
mkdir -p $L
say() { echo "$(date +%T) $*"; }
cpus_busy() {
	awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {
		b = 100 - $NF; c = $2 + 0
		if (c >= 8 && c <= 55) { w += b; nw++; if (b > mw) mw = b }
		else { r += b; nr++; if (b > mr) mr = b } }
	END { printf "work CPUs %.1f%% (max %.0f%%), ring CPUs %.1f%% (max %.0f%%)", w / nw, mw, r / nr, mr }' "$1"
}
on_rings() {	# connections placed on ring CPUs, and in all, from a placed line
	tr ' ' '\n' < "$1" | awk -F: '$2 != "" { c = $1 + 0; t += $2; if (c < 8 || c > 55) r += $2 }
		END { printf "%d of %d connections on ring CPUs", r, t }'
}
for run in "$@"; do
	IFS=: read n spec <<< "$run"
	[ -n "$n" ] && [ -n "$spec" ] || { say "ab-spill.sh <n>:<off|w<k>> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	(	W=/var/tmp/zc-run-logs/t2-r$n/run/wire-sec.tsv
		for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 10 ] && break; sleep 0.2; done
		grep '^placed' /sys/kernel/debug/nfsd/rtc_stats > $L/placed-$n.txt ) &
	sampler=$!
	case $spec in
	off)	./step.sh $L spill-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1 ;;
	w*)	EXP_RTC_SPILL_CPUS=$RINGS EXP_RTC_SPILL_WEIGHT=${spec#w} \
		./step.sh $L spill-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1 ;;
	*)	say "spec $spec: off or w<k>"; exit 1 ;;
	esac
	rc=$?
	wait $sampler
	./settled.sh apply > $L/settled-after-$n.txt 2>&1
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n spill $spec rc $rc | $(cpus_busy /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt) | $(on_rings $L/placed-$n.txt) | spill in run: $(grep -oE 'svc_rtc_spill_cpus=[0-9,-]+ ?(svc_rtc_spill_weight=[0-9]+)?' $L/t2-r$n.log | head -1) | after: $(tail -1 $L/settled-after-$n.txt)"
done
