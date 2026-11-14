#!/bin/bash
# run638.sh: round 103, two-port NFS reads on all eight drives (round
# 102's t2-r633 and r637: 54 GB/s, neither the drives nor a port at its
# limit), with each server port's per-ring transmit counters read after
# the run.  RUNS is <n>:<rw>:<xps> in turn, <xps> "cpus" (the settled
# map, tx-i on one nfsd CPU) or "rxqs" (twoport.sh's XPS=rxqs).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
for run in ${RUNS:-638:read:cpus}; do
	IFS=: read n rw xps <<< "$run"
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	X=; [ "$xps" = rxqs ] && X=rxqs
	# Each connection's transmit queue, sampled in the run's window
	# (twoport.sh counts 10 s from about WARM + 20 s after its start).
	( sleep 75; for p in enp161s0f0 enp164s0f0; do
		bpftrace -q -e "kprobe:xeu_start_xmit { \$s = (struct sk_buff *)arg0; if (\$s->sk != 0) { @q[str(\$s->dev->name), \$s->queue_mapping, \$s->sk] = count(); } } interval:s:3 { exit(); }" 2>&1 |
			awk -v p=$p '$0 ~ p' ; done > $O/txq-sk.txt ) &
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
		WARM=10 RUNTIME=40 RW=$rw RWTSV=$O/rw.tsv TAG=8d-ab-$rw-$xps XPS=$X \
		WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; wait; exit 1; }
	wait
	for d in enp161s0f0 enp164s0f0; do ethtool -S $d > $O/srv-ethtool-$d.txt; done
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n ab $rw xps $xps | $(grep '^total' $O/run.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/run.log | awk '{s+=$3} END {print s}') | $(grep -o 'no drive stepped down' $O/run.log || echo 'a drive stepped down') | new kernel warnings $hung"
	grep -E '^  (read|write):|^port |drives of' $O/run.log | sed 's/^/    /'
	for d in enp161s0f0 enp164s0f0; do
		echo "    $d tx GB by ring: $(awk '$1 ~ /^ring_[0-9]+_tx_bytes:$/ {printf "%.0f ", $2/1e9}' $O/srv-ethtool-$d.txt)| sockets by queue: $(awk -v d=$d '$0 ~ d {gsub(/[@q\[\],:]/, " "); print $2}' $O/txq-sk.txt | sort | uniq -c | awk '{printf "q%s:%s ", $2, $1}')"
	done
	echo "    ring CPUs busy: $(awk '$2 ~ /^[0-9]+$/ && ($2 < 8 || $2 > 55) {u[$2] += 100 - $NF; c[$2]++} END {for (k in u) printf "%s:%.0f ", k, u[k]/c[k]}' $O/run/srv-mpstat.txt)"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
