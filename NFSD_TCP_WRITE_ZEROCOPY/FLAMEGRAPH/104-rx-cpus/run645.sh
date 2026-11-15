#!/bin/bash
# run645.sh: round 104, what the receive rings' CPUs do on two-port
# reads at the settled xps_rxqs (round 103: 81 to 93% busy, all in
# softirq).  RUNS is <n>:<rw> in turn.  In each run's window it profiles
# the sixteen ring CPUs (0-7, 56-63) with call graphs for 8 s and reads
# each port's per-ring counters and /proc/softirqs either side of it.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
snap() {	# snap <dir> <tag>
	for d in enp161s0f0 enp164s0f0; do ethtool -S $d > $1/ethtool-$d.$2; done
	cat /proc/softirqs > $1/softirqs.$2; cat /proc/zoneinfo > $1/zoneinfo.$2; cat /sys/kernel/debug/nfsd/rtc_stats > $1/rtc_stats.$2 2>/dev/null; date +%s.%N > $1/time.$2
}
for run in ${RUNS:-647:read}; do
	IFS=: read n rw hf rtc rc rb rfs txd <<< "$run"
	# <txd>: net.core.skb_tx_defer_max for the run (on a kernel with
	# 7bd353fb130ef), through settled.sh's EXP_SKB_TX_DEFER_MAX.
	if [ -n "$txd" ]; then export EXP_SKB_TX_DEFER_MAX=$txd; else unset EXP_SKB_TX_DEFER_MAX; fi
	# <rfs>: RFS flow table entries for the run (t2-r687 on), 0 for off,
	# through settled.sh's EXP_RFS.  With RESTART=1 nfs-server restarts
	# before the run, so the rtc placement's receive-CPU mark starts empty.
	if [ -n "$rfs" ]; then export EXP_RFS=$rfs; else unset EXP_RFS; fi
	[ "${RESTART:-0}" = 1 ] && { client_free && systemctl restart nfs-server || exit 1; }
	# <rb>: sunrpc's svc_rtc_balance for the run (t2-r682 on), through
	# settled.sh's EXP_RTC_BALANCE.
	if [ -n "$rb" ]; then export EXP_RTC_BALANCE=$rb; else unset EXP_RTC_BALANCE; fi
	# <rc>: sunrpc's svc_rtc_cpus for the run (t2-r676 on), "none" for the
	# empty default, through settled.sh's EXP_RTC_CPUS.
	if [ -n "$rc" ]; then export EXP_RTC_CPUS=$rc; else unset EXP_RTC_CPUS; fi
	# <rtc>: nfsd's rtc_read for the run (t2-r658 on; settled.sh holds N),
	# through settled.sh's EXP_RTC_READ to apply and to twoport.sh's audit.
	if [ -n "$rtc" ]; then export EXP_RTC_READ=$rtc; else unset EXP_RTC_READ; fi
	# <hf>: vm.percpu_pagelist_high_fraction for the run (t2-r651 on), put
	# back to 0 after the last.
	[ -n "$hf" ] && { sysctl -q -w vm.percpu_pagelist_high_fraction=$hf || exit 1; }
	[ "$(df --output=avail -k / | tail -1)" -gt 1572864 ] || { say "root has under 1.5 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	# In the load, not before it: twoport.sh places its flows with a write
	# job first, and its samplers write wire-sec.tsv one row a second from
	# the load's first second, so wait for its 25th row (15 s into what
	# fio counts at WARM=10).  t2-r645 slept 75 s and profiled the
	# placement instead.
	( W=$O/run/wire-sec.tsv
	  for i in $(seq 600); do [ -f $W ] && [ $(wc -l < $W) -ge 26 ] && break; sleep 0.5; done
	  snap $O 0
	  if [ "${PROBE:-perf}" = none ]; then
		sleep 8
	  elif [ "${PROBE:-perf}" = rtc ]; then
		# Where the rtc work items run (t2-r675 on): runs and time by CPU,
		# each connection's chosen CPU, and every placement's receive CPU,
		# whether it came from the softirq, and the CPU it chose; then 5 s
		# of every CPU sampled by task and symbol.
		bpftrace -q -e '
		kprobe:svc_rtc_work { @t[tid] = nsecs; @runs[cpu] = count(); @conn[arg0, ((struct svc_rtc_conn *)arg0)->cpu, cpu] = count(); }
		kretprobe:svc_rtc_work /@t[tid]/ { @us[cpu] = sum((int64)((nsecs - @t[tid]) / 1000)); delete(@t[tid]); }
		kprobe:svc_rtc_place { @place_rx[arg2, arg3] = count(); }
		kretprobe:svc_rtc_place { @place_ret[retval] = count(); }
		interval:s:3 { clear(@t); exit(); }' > $O/rtc-place.txt 2>&1
		perf record -q -a -F 999 -o /dev/shm/zc-rx.data -- sleep 5 > $O/perf.out 2>&1
		perf report -i /dev/shm/zc-rx.data --no-children --sort cpu,comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . > $O/perf-cpu-comm.txt
		perf report -i /dev/shm/zc-rx.data --no-children --sort comm,sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -120 > $O/perf-comm-sym.txt
	  elif [ "${PROBE:-perf}" = lock ]; then
		# How long the zone's free and refill take, and who waits on its lock.
		perf lock contention -a -b -E 20 -- sleep 4 > $O/lock-contention.txt 2>&1
		bpftrace -q -e '
		kprobe:free_pcppages_bulk { @t[tid] = nsecs; @pages[func, cpu < 8 || cpu > 55 ? "ring" : "nfsd"] = sum(arg1); }
		kprobe:rmqueue_bulk { @t[tid] = nsecs; @pages[func, cpu < 8 || cpu > 55 ? "ring" : "nfsd"] = sum(arg2); }
		kretprobe:free_pcppages_bulk, kretprobe:rmqueue_bulk /@t[tid]/ { @us[func, cpu < 8 || cpu > 55 ? "ring" : "nfsd"] = stats((uint64)(nsecs - @t[tid]) / 1000); @ns_total[func, cpu < 8 || cpu > 55 ? "ring" : "nfsd"] = sum(nsecs - @t[tid]); delete(@t[tid]); }
		interval:s:3 { exit(); }' > $O/zone-bulk.txt 2>&1
	  else
		perf record -q -g -C 0-7,56-63 -F 1999 -o /dev/shm/zc-rx.data -- sleep 8 > $O/perf.out 2>&1
	  fi
	  snap $O 1
	  echo "profile $(cat $O/time.0) to $(cat $O/time.1), load from $(awk 'NR == 2 {print $2}' $W)" > $O/when.txt ) &
	env FS_A="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" FS_B="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7" \
		WARM=10 RUNTIME=40 RW=$rw RWTSV=$O/rw.tsv TAG=8d-ab-$rw-rxcpu \
		WRITES=$TWO_WRITES BUDGET_PAGES=8198 ./twoport.sh $O/run ab > $O/run.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; wait; exit 1; }
	wait
	dmesg > $O/dmesg.1
	[ -f /dev/shm/zc-rx.data ] && perf report -i /dev/shm/zc-rx.data --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/perf-cpu.txt
	perf report -i /dev/shm/zc-rx.data --no-children -g none --percent-limit 0.3 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -80 > $O/perf-rx-self.txt
	perf report -i /dev/shm/zc-rx.data --children -g none --percent-limit 2 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -100 > $O/perf-rx-children.txt
	perf report -i /dev/shm/zc-rx.data --no-children -g caller,0.5,callee --percent-limit 1.5 --sort symbol --cpu 0,2,4,6,56,58,60,62 2>/dev/null | grep -v '^$' | head -600 > $O/perf-rx-callers.txt
	perf script -i /dev/shm/zc-rx.data -F cpu,ip,sym 2>/dev/null | xz -T8 -3 > $O/perf-g.script.xz
	rm -f /dev/shm/zc-rx.data
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n ab $rw | $(grep '^total' $O/run.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/run.log | awk '{s+=$3} END {print s}') | $(grep -o 'no drive stepped down' $O/run.log || echo 'a drive stepped down') | new kernel warnings $hung"
	grep -E '^  (read|write):|^port |drives of' $O/run.log | sed 's/^/    /'
	echo "    ring CPUs busy: $(awk '$2 ~ /^[0-9]+$/ && ($2 < 8 || $2 > 55) {u[$2] += 100 - $NF; c[$2]++} END {for (k in u) printf "%s:%.0f ", k, u[k]/c[k]}' $O/run/srv-mpstat.txt)"
	echo "    $(cat $O/when.txt) | high_fraction $(cat /proc/sys/vm/percpu_pagelist_high_fraction)"
	[ -n "$rtc" ] && echo "    rtc_read $(cat /sys/kernel/debug/nfsd/rtc_read 2>/dev/null) in the run; rtc_stats in the window: $(join -j1 <(sort $O/rtc_stats.0) <(sort $O/rtc_stats.1) | awk '$3 != $2 {printf "%s %d, ", $1, $3 - $2}')"
	[ -e /sys/module/sunrpc/parameters/svc_rtc_cpus ] && echo "    svc_rtc_cpus \"$(cat /sys/module/sunrpc/parameters/svc_rtc_cpus)\"; busiest CPUs: $(awk '$2 ~ /^[0-9]+$/ {u[$2] += 100 - $NF; c[$2]++} END {for (k in u) printf "%s %.0f\n", k, u[k]/c[k]}' $O/run/srv-mpstat.txt | sort -k2 -n -r | head -8 | awk '{printf "%s:%s ", $1, $2}')"
	echo "    nfsd CPUs busy: mean $(awk '$2 ~ /^[0-9]+$/ && $2 >= 8 && $2 <= 55 {u += 100 - $NF; c++} END {printf "%.0f%%", u / c}' $O/run/srv-mpstat.txt)"
	echo "    per-CPU high in the load (zone Normal): ring CPUs $(awk '/^Node 0, zone   Normal/ {z=1} z && /^Node 0, zone  Movable/ {z=0} z && /cpu: / {c=$2} z && /^ +high:/ && (c < 8 || c > 55) {printf "%s:%s ", c, $2}' $O/zoneinfo.0)"
	echo "      nfsd CPUs from $(awk '/^Node 0, zone   Normal/ {z=1} z && /^Node 0, zone  Movable/ {z=0} z && /cpu: / {c=$2} z && /^ +high:/ && c >= 8 && c <= 55 {print $2}' $O/zoneinfo.0 | sort -n | sed -n '1p;$p' | paste -sd ' ' | sed 's/ / to /')"
	for d in enp161s0f0 enp164s0f0; do
		echo "    $d sends by queue: $(awk -v d=$d '$0 ~ d {gsub(/[@q\[\],:]/, " "); n[$2] += $NF; t += $NF} END {for (k in n) printf "q%s %.0f%% ", k, 100 * n[k] / t}' $O/txq-sk.txt)| sockets by queue: $(awk -v d=$d '$0 ~ d {gsub(/[@q\[\],:]/, " "); print $2}' $O/txq-sk.txt | sort | uniq -c | awk '{printf "q%s:%s ", $2, $1}')"
	done
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
sysctl -q -w vm.percpu_pagelist_high_fraction=0
unset EXP_RTC_READ EXP_RTC_CPUS EXP_RTC_BALANCE EXP_RFS EXP_SKB_TX_DEFER_MAX
[ "${RESTART:-0}" = 1 ] && systemctl restart nfs-server
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
