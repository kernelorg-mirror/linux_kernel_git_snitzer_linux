#!/bin/bash
# ab-spill.sh: FLAMEGRAPH round 154, nfsd's spill onto the ring CPUs or
# none (Mike, 19 November: "should we try _not_ spilling to the rings
# CPUs?  We did that to give more CPU headroom for NFSD work, but if it is
# causing thrashing on the rings CPUs is it getting in the way?", then
# "yes, queue round 154 after t2-r1210").  Settled Release 17, 4 rings,
# no reload.  Arms:
#   S  settled-iops.sh's spill: svc_rtc_spill_cpus 0-7,56-63, weight 1
#   N  no spill: EXP_RTC_SPILL_CPUS=none, nfsd on 8-55 only
# S N S N, t2-r1211 to r1214, each ../151-packets/packets.sh (IOPS at
# iodepth 96 with the per-READ counts and each ring's frames a second),
# and from its mpstat the ring CPUs' busy and nfsd share.  Spill is held
# only by settled-iops.sh, so the WRITE and READ passes are not changed.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in $(seq 1211 1214); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
until grep -q "ab-pair2 all done" $G/ab-pair2b-153.log; do sleep 5; done
say "round 153 done; xeu $(cat /sys/module/xeu/srcversion), $(./settled.sh audit rest 2>&1 | tail -1)"
io() {	# io <n> <arm>
	case $2 in S) unset EXP_RTC_SPILL_CPUS ;; N) export EXP_RTC_SPILL_CPUS=none ;; esac
	./step.sh $G pk-$1 ../FLAMEGRAPH/151-packets/packets.sh $1 > $G/t2-r$1.log 2>&1
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
	grep -E "softirq_NET_RX_(ring|other)|softnet_rps_ipis_other|^ring rx" $L/t2-r$1/packets.txt | sed 's/^/    /'
	awk '$2 ~ /^[0-9]+$/ { b = 100 - $NF; if ($2 < 8 || $2 > 55) { rb += b; rs += $5; rn++; if (b > mx) mx = b; if (rn == 1 || b < mn) mn = b } else { nb += b; nn++ } }
	     END { printf "    ring CPUs busy %.0f%% (%.0f to %.0f), nfsd work on them %.0f%%; nfsd CPUs busy %.0f%%\n", rb / rn, mn, mx, rs / rn, nb / nn }' $L/t2-r$1/run/srv-mpstat.txt
}
for x in 1211:S 1212:N 1213:S 1214:N; do io ${x%:*} ${x#*:}; done
unset EXP_RTC_SPILL_CPUS
say "after: $(./settled.sh audit rest 2>&1 | tail -1)"
say "ab-spill all done"
