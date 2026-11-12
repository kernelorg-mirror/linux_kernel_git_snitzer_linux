#!/bin/bash
# run524.sh: round 98, the drives' queue depth read exactly (inflight.py,
# /sys/block/nvmeNn1/inflight every ms for 12 s of the window) with the
# mesh's memory requests beside it, in two runs: (1) the settled two-port
# loan load (t2-r524); (2) one port, a, loading its four drives (t2-r525)
# while the other four, nvme4 to nvme7, take a local flat fio (3 x 1 MiB
# a drive, 12 s) at the same time: whether the NIC's traffic slows drives
# it is not writing to (10.20 GB/s each alone).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
HERE=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/98-two-ceilings
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(cat /sys/module/sunrpc/parameters/svc_tcp_rx_target_window)" = 0 ] || { say "the window is not 0"; exit 1; }
stop() { echo N > $MARK; ./settled.sh apply > /dev/null 2>&1; exit 1; }
# From the window's start (twoport writes perf-stat-window.txt as it opens
# it): the trace on CPU 63 with the region's sampler.
split() {	# split <run dir>
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	date +%s.%N > $O/mesh.t0
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mesh.txt -- sleep 10 > /dev/null 2>&1 &
	[ "$LOCAL" = local ] && ( fio --ioengine=psync --direct=1 --rw=write --bs=1M --size=2G --time_based --runtime=12 --numjobs=3 --output-format=json --output=$O/local-fio.json $(for d in 4 5 6 7; do mkdir -p /hs/nvme$d/local; echo --name=d$d --directory=/hs/nvme$d/local; done) > /dev/null 2>&1; for d in 4 5 6 7; do rm -rf /hs/nvme$d/local; done ) &
	taskset -c 63 python3 -I $HERE/inflight.py $O/inflight.txt 12
	date +%s.%N > $O/$BT.t1
	wait
}
n=${N0:-524}
for cell in ${CELLS:-ab:none a:local}; do
	PORTS=${cell%:*}; LOCAL=${cell#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	split $O &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=ceil-$PORTS-$LOCAL WRITES=$TWO_WRITES REGION_SAMPLE=1 ./twoport.sh $O/pmu $PORTS > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n $PORTS $LOCAL | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt 2>/dev/null) | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	sed 's/^/    /' $O/inflight.txt
	[ -e $O/local-fio.json ] && python3 -I - $O/local-fio.json <<'PY' | sed 's/^/    /'
import json, sys
j = json.load(open(sys.argv[1])); per = {}
for job in j['jobs']: per[job['jobname']] = per.get(job['jobname'], 0) + job['write']['bw_bytes']
print('local fio on nvme4-7 beside the load: ' + ' '.join(f"{k} {per[k]/1e9:.2f}" for k in sorted(per)) + f" GB/s, total {sum(per.values())/1e9:.2f}")
PY
	python3 -I ../FLAMEGRAPH/93-l3-working-set/l3read.py t2-r$n 2>&1 | sed 's/^/    /'
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
