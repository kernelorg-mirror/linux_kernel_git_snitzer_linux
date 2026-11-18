#!/bin/bash
# ab-clnt.sh <n>:<one|settled> ...: FLAMEGRAPH round 117, one RPC client a
# port at nconnect 64 (Mike, 18 November: "after round 116, run one mount
# per port at nconnect 64"), David Flynn's shape (one mount, nconnect 128,
# one rpc_clnt behind patch 27's shards, 7cc54f28c69fc in build 541).
# "one": settled-iops.sh with SRV_ALIAS=0 NCONN=64, so a port's four
# filesystems are mounted from its one server address and share one
# nfs_client, one rpc_clnt and 64 connections (128 in all).  "settled":
# settled-iops.sh as held (SRV_ALIAS=1 NCONN=12, eight rpc_clnts, 96).
# At the load's second 10 the server's established NFS connections are
# counted a port, and rtc_stats' placed line saved.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${L:-/var/tmp/zc-run-logs/r117}
mkdir -p $L
say() { echo "$(date +%T) $*"; }
cpus_busy() {
	awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {
		b = 100 - $NF; c = $2 + 0
		if (c >= 8 && c <= 55) { w += b; nw++; if (b > mw) mw = b }
		else { r += b; nr++; if (b > mr) mr = b } }
	END { printf "work CPUs %.1f%% (max %.0f%%), ring CPUs %.1f%% (max %.0f%%)", w / nw, mw, r / nr, mr }' "$1"
}
per_cpu() {	# connections a work CPU: least, most
	tr ' ' '\n' < "$1" | awk -F: '$2 != "" { if (!n++ || $2 < lo) lo = $2; if ($2 > hi) hi = $2; t += $2 }
		END { printf "%d placed over %d CPUs, %d to %d a CPU", t, n, lo, hi }'
}
for run in "$@"; do
	IFS=: read n spec <<< "$run"
	[ -n "$n" ] && [ -n "$spec" ] || { say "ab-clnt.sh <n>:<one|settled> ..."; exit 1; }
	[ -e /var/tmp/zc-run-logs/t2-r$n ] && { say "t2-r$n exists"; exit 1; }
	(	W=/var/tmp/zc-run-logs/t2-r$n/run/wire-sec.tsv
		for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 10 ] && break; sleep 0.2; done
		grep '^placed' /sys/kernel/debug/nfsd/rtc_stats > $L/placed-$n.txt
		{ echo "port a $(ss -Htn state established '( sport = :2049 )' src 192.168.0.0/24 | wc -l)"
		  echo "port b $(ss -Htn state established '( sport = :2049 )' src 192.168.1.0/24 | wc -l)"; } > $L/conns-$n.txt ) &
	sampler=$!
	case $spec in
	one)	SRV_ALIAS=0 NCONN=64 ./step.sh $L clnt-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1 ;;
	settled) ./step.sh $L clnt-$n ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n > $L/t2-r$n.log 2>&1 ;;
	*)	say "spec $spec: one or settled"; exit 1 ;;
	esac
	rc=$?
	wait $sampler
	./settled.sh apply > $L/settled-after-$n.txt 2>&1
	say "$(grep -E "t2-r$n " $L/t2-r$n.log | tail -1 | cut -d' ' -f2-)"
	say "t2-r$n $spec rc $rc | $(cpus_busy /var/tmp/zc-run-logs/t2-r$n/run/srv-mpstat.txt) | connections $(paste -sd' ' $L/conns-$n.txt) | $(per_cpu $L/placed-$n.txt) | after: $(tail -1 $L/settled-after-$n.txt)"
done
