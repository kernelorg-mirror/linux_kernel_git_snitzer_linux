#!/bin/bash
# run558.sh: round 99, the held receive buffer and the connections a port
# at the settled window 7 and budget 64.  At t2-r557 the client's
# connections were held by the server's advertised window 97% of the
# time they were busy (snd_wnd about 240 KB, round trip 75 us: 3.2 GB/s
# a connection, 78 on 24), and both settings were chosen when memory or
# the client was the wall.  For each RCVBUF:NCONN in CELLS, one settled
# two-port run (WARM=10 RUNTIME=40, inside the drives' fast region) with,
# from the window's start, the drives' exact queue depth (round 98's
# inflight.py, 12 s) and the mesh's memory requests (10 s); then the
# client's connections' rwnd_limited share and mean snd_wnd from
# twoport's ss-client.txt, and l3read.py's held and memory line.  WRITES
# (64) is passed on: twoport.sh copies without it (t2-r558..r560's first
# try, kept as .copy-mode-stopped), and a run without loans at window 7
# stops the sweep.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
FG=/home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH
MARK=/sys/module/page_pool/parameters/rx_region_held_mark
SP=/sys/module/sunrpc/parameters
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ -e $SP/svc_tcp_rx_target_window ] || { say "this sunrpc has no svc_tcp_rx_target_window"; exit 1; }
stop() { echo N > $MARK; ./settled.sh apply > /dev/null 2>&1; exit 1; }
side() {	# side <run dir>: depth and mesh from the window's start
	local O=$1 i
	for i in $(seq 1 30000); do [ -e $O/pmu/perf-stat-window.txt ] && break; sleep 0.01; done
	[ -e $O/pmu/perf-stat-window.txt ] || return
	date +%s.%N > $O/mesh.t0
	perf stat -a -x, -e arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/mesh.txt -- sleep 10 > /dev/null 2>&1 &
	taskset -c 63 python3 -I $FG/98-two-ceilings/inflight.py $O/inflight.txt 12
	wait
}
rwnd() {	# rwnd <ss file>: summed rwnd_limited over busy, mean snd_wnd
	python3 -I - "$1" <<'E'
import re, sys
b = r = 0; w = []
for l in open(sys.argv[1]):
    m = re.search(r'busy:(\d+)ms', l)
    if m:
        b += int(m.group(1))
        x = re.search(r'rwnd_limited:(\d+)ms', l); r += int(x.group(1)) if x else 0
    m = re.search(r'snd_wnd:(\d+)', l)
    if m: w.append(int(m.group(1)))
print(f'client: {len(w)} connections, rwnd_limited {100 * r / b:.1f}% of busy, '
      f'snd_wnd mean {sum(w) / len(w) / 1024:.0f} KiB' if b and w else 'client: no ss')
E
}
n=${N0:-558}
for c in ${CELLS:-524288:12 655360:12 786432:12 524288:16 786432:16 524288:12}; do
	rb=${c%:*}; nc=${c#*:}
	[ "$(df --output=avail -k / | tail -1)" -gt 1048576 ] || { say "root has under 1 GB free"; stop; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; stop; }
	echo Y > $MARK
	./settled.sh apply > $O/apply-pmu.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply-pmu.txt | head -3)"; stop; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; stop; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	side $O &
	env WARM=10 RUNTIME=40 TSV=$O/results.tsv TAG=rcvbuf-$rb-nconn-$nc WRITES=${WRITES:-64} RCVBUF=$rb NCONN=$nc REGION_SAMPLE=1 ./twoport.sh $O/pmu ab > $O/pmu.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/pmu.log | head -3)"; wait; stop; }
	wait
	grep -q 'budget [0-9]* pages for [1-9][0-9]* WRITEs.*window 7$' $O/pmu/geometry.txt || { say "t2-r$n did not loan at window 7: $(grep '^loan' $O/pmu/geometry.txt)"; stop; }
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG|Failed to allocate')
	say "t2-r$n rcvbuf $rb nconn $nc | $(grep '^total' $O/pmu.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/pmu.log | awk '{s+=$3} END {print s}') | flows placed $(grep -c "^flow" $O/pmu/place-a.txt)/$(grep -c "^flow" $O/pmu/place-b.txt) | $(grep -o 'no drive stepped down' $O/pmu.log || echo 'a drive stepped down') | new kernel warnings $hung"
	echo "    $(grep '^this run' $O/pmu/geometry.txt | grep -oE 'rcvbuf [0-9]+|nconn [0-9]+' | tr '\n' ' '); $(grep -oE 'window [0-9]+$' $O/pmu/geometry.txt | tail -1); $(grep '^loans' $O/pmu/window.txt); $(grep '^client retransmits' $O/pmu/window.txt)"
	echo "    $(rwnd $O/pmu/ss-client.txt)"
	tail -1 $O/inflight.txt | sed 's/^/    drives /'
	echo "    $(grep -o 'PAST THE FAST REGION.*' $O/pmu/drives.txt | cut -c1-40)"
	echo "    $(python3 -I $FG/93-l3-working-set/l3read.py t2-r$n 2>&1 | cut -d'|' -f3- | tr -s ' ')"
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; stop; }
	n=$((n + 1))
done
echo N > $MARK
./settled.sh apply > $L/t2-r$((n - 1))/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$((n - 1))/apply-after.txt)"
