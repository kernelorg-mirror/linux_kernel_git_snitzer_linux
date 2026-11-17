#!/bin/bash
# nfs4k.sh <n>[:<iodepth>[:<nconnect>]] ...: FLAMEGRAPH round 108, 4 KiB
# random READs over NFS (Mike, 17 November: "then the NFS 4K runs").  Each
# run is twoport.sh on both ports at the settled setting, reading the
# kept f-* files (about 2 GB a drive, the same local.sh read): fio on
# the client 16 jobs a port, 4 KiB random reads, O_DIRECT, io_uring with
# registered buffers, at <iodepth> a job and nconnect <nconnect> a port
# (common.sh's TWO_NCONN when not given), 30 s of rest, WARM=5.  An
# iodepth or nconnect left out is the environment's IODEPTH or NCONN,
# which xeu-rig/settled-iops.sh run sets to the settled 64 and 12 (Mike,
# 17 November); before that an iodepth was required.
# RUNTIME=20.  From the load's second 8, 10 s of mpstat on the server and
# the client.  It prints IOPS in all and a port, completion latency, and
# how busy each end was (iowait not counted).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
busy() {	# busy <mpstat file> <first cpu> <last cpu>
	awk -v a=$2 -v b=$3 '$1 == "Average:" && $2 == "CPU" {for (i = 1; i <= NF; i++) c[$i] = i}
		$1 == "Average:" && $2 ~ /^[0-9]+$/ && $2 >= a && $2 <= b {v = 100 - $c["%idle"] - $c["%iowait"]; s += v; k++; if (v > m) m = v}
		END {printf "%.0f%% (busiest %.0f%%)", k ? s / k : 0, m}' $1
}
for run in "$@"; do
	IFS=: read n qd nc <<< "$run"
	qd=${qd:-$IODEPTH} nc=${nc:-$NCONN}
	[ -n "$n" ] && [ -n "$qd" ] || { say "nfs4k.sh <n>[:<iodepth>[:<nconnect>]] ... (IODEPTH from the environment)"; exit 1; }
	[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sleep ${REST:-30}
	dmesg > $O/dmesg.0
	(
		W=$O/run/wire-sec.tsv; for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
		$SSHC "mpstat -P ALL 1 10" > $O/cli-mpstat-w.txt 2>&1 &
		mpstat -P ALL 1 10 > $O/srv-mpstat-w.txt 2>&1
		wait
	) &
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
		WARM=${WARM:-5} RUNTIME=${RUNTIME:-20} RW=randread BS=4k IODEPTH=$qd ${nc:+NCONN=$nc} RSIZE=262144 \
		IOENGINE=io_uring FIXEDBUFS=1 KEEP_FILES=1 RWTSV=$O/rw.tsv TAG=nfs4k-$n \
		WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1
	rc=$?
	wait
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	iops=$(awk '/^  read:/ {sub(/k$/, "", $4); s += $4} END {printf "%.2f", s / 1000}' $O/run.log)
	ports=$(awk '/^  read:/ {sub(/k$/, "", $4); printf "%s%.2f", n++ ? " / " : "", $4 / 1000}' $O/run.log)
	lat=$(awk '/^  read:/ {printf "%s%s / %s", n++ ? "; " : "", $7, $10}' $O/run.log)
	say "t2-r$n 4k randread iodepth $qd nconnect $(grep -oE 'nconn [0-9]+' $O/run/geometry.txt | head -1 | cut -d' ' -f2): exit $rc, $iops M IOPS (ports $ports M), completion ms mean / p99 $lat | server busy $(busy $O/srv-mpstat-w.txt 0 63) | client busy $(busy $O/cli-mpstat-w.txt 0 47) | $(grep -iE 'files (kept|written)' $O/run/geometry.txt | head -1 | cut -c1-22) | new kernel warnings $hung"
done
./settled.sh apply > $L/r108-nfs4k-after.txt 2>&1; say "after: $(tail -1 $L/r108-nfs4k-after.txt)"
