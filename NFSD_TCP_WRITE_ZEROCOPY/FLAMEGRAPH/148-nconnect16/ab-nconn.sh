#!/bin/bash
# ab-nconn.sh: FLAMEGRAPH round 148, nconnect 16 a mount against the
# settled 12 (Mike, 19 November: "yes, run the nconnect 16 A/B after the
# WRITE passes").  The client on its updates/ stack (round 145's).  4 KiB
# random READs at iodepth 96, nconnect 12 16 12 16 12 16 (t2-r1185 to
# r1190): 96 connections against 128, eight mounts.  After each, the
# server's work CPUs (8-55) by mpstat: mean, 10th percentile, and how
# many at 95% or more and at 70% or less.  No code or settled setting
# changes, so no passes.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541
say() { echo "$(date +%T) $*"; }
for n in 1185 1186 1187 1188 1189 1190; do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
for x in 1185:12 1186:16 1187:12 1188:16 1189:12 1190:16; do
	n=${x%:*}; c=${x#*:}
	./step.sh $G nc-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:96:$c > $G/t2-r$n.log 2>&1
	line=$(grep -E "t2-r$n " $G/t2-r$n.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$n -v a=$c '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) q = $(i + 1) } } sub("%", "", s); sub("%", "", q); printf "t2-r%s nconnect %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, q / 100 * 48 / m }'
	awk -v n=$n '$2 ~ /^[0-9]+$/ && $1 != "Average:" && $2 >= 8 && $2 <= 55 { k[$2]++; u[$2] += 100 - $NF }
		END { for (c in k) { b = u[c] / k[c]; t++; s += b; v[t] = b; if (b >= 95) h++; if (b <= 70) l++ }
		asort(v); printf "t2-r%s work CPUs: mean %.0f%%, p10 %.0f%%, at 95%%+ %d, at 70%%- %d\n", n, s / t, v[int(t / 10) + 1], h, l }' $L/t2-r$n/srv-mpstat-w.txt
done
./settled.sh audit rest 2>&1 | tail -1
say "ab-nconn all done"
