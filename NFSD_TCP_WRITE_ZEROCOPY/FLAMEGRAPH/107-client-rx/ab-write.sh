#!/bin/bash
# ab-write.sh <n>:<engine>[:fixed] ...: round 107's A/B of fio's engine for
# WRITEs (Mike, 17 November: "run the write A/B with io_uring fixedbufs"),
# in turns.  Each run is the WRITE pass (xeu-rig/write-check.sh: a trim, 30
# s of rest, WARM=3 RUNTIME=15 on all eight drives, files kept) with
# twoport.sh's IOENGINE=<engine> and FIXEDBUFS=1 with "fixed".  In the
# load, by the wire's per-second rows: from second 4, 5 s of mpstat on the
# client and on the server and of the client's memory controllers; from
# second 10, 5 s of the client's call chains at 499 Hz.  It prints the
# pass's verdict and rate, how busy each end was (iowait not counted),
# the client's memory a byte, and the client's shares of copying
# (memcpy), pinning (try_grab_folio_fast), the NFS direct write's
# submission (nfs_direct_write_schedule_iovec) and its send
# (xs_tcp_send_request).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
busy() {	# busy <mpstat file> <first cpu> <last cpu>: mean of 100 - idle - iowait
	awk -v a=$2 -v b=$3 '$1 == "Average:" && $2 == "CPU" {for (i = 1; i <= NF; i++) c[$i] = i}
		$1 == "Average:" && $2 ~ /^[0-9]+$/ && $2 >= a && $2 <= b {s += 100 - $c["%idle"] - $c["%iowait"]; k++}
		END {printf "%.0f%%", k ? s / k : 0}' $1
}
for run in "$@"; do
	IFS=: read n eng fx <<< "$run"
	[ -n "$n" ] && [ -n "$eng" ] || { echo "ab-write.sh <n>:<engine>[:fixed] ..."; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	(
		row() { W=$O/run/wire-sec.tsv; for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge $1 ] && return 0; sleep 0.2; done; return 1; }
		row 4 || exit 0
		$SSHC "mpstat -P ALL 1 5" > $O/cli-mpstat-w.txt 2>&1 &
		mpstat -P ALL 1 5 > $O/srv-mpstat-w.txt 2>&1 &
		$SSHC "perf stat -a --per-socket -x, -e uncore_imc/cas_count_read/,uncore_imc/cas_count_write/ -- sleep 5" > $O/cli107-mem.txt 2>&1
		wait
		row 10 || exit 0
		$SSHC "perf record -q -a -g -F 499 -o /dev/shm/zc-cliw.data -- sleep 5 > /dev/null 2>&1"
		R="perf report -i /dev/shm/zc-cliw.data"
		$SSHC "$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -150" > $O/cli-perf-self.txt
		$SSHC "$R --children -g none --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli-perf-children.txt
		$SSHC "$R --no-children -g none --sort comm --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep ." > $O/cli-perf-comm.txt
		$SSHC "rm -f /dev/shm/zc-cliw.data"
	) &
	FB=; [ "$fx" = fixed ] && FB=1
	IOENGINE=$eng FIXEDBUFS=$FB ./write-check.sh $n > $O/ab-write.out 2>&1
	wait
	gb=$(grep -m1 '^total' $O/run.log | grep -oE '[0-9.]+' | head -1)
	mem=$(awk -F, '/cas_count/ {s+=$3} END {printf "%.1f", s * 1048576 / 5e9}' $O/cli107-mem.txt)
	pct() { awk -v s="$2" '$NF == s {print $1; exit}' $O/$1; }
	echo "$(date +%T) t2-r$n $eng${fx:+ $fx}: $(grep -oE 'write check: [A-Z]+' $O/ab-write.out) $gb GB/s | client busy $(busy $O/cli-mpstat-w.txt 0 23) / $(busy $O/cli-mpstat-w.txt 24 47), server busy $(busy $O/srv-mpstat-w.txt 0 63) | client memory $mem GB/s, $(awk -v m=$mem -v g=$gb 'BEGIN {printf "%.2f", m / g}') a byte | memcpy $(pct cli-perf-self.txt __pi_memcpy) pinning $(pct cli-perf-self.txt try_grab_folio_fast) submission $(pct cli-perf-children.txt nfs_direct_write_schedule_iovec) send $(pct cli-perf-children.txt xs_tcp_send_request) | $(grep -oE '(no drive stepped down|a drive stepped down)' $O/ab-write.out | head -1), $(grep -oE 'new kernel warnings [0-9]+' $O/ab-write.out | head -1)"
done
