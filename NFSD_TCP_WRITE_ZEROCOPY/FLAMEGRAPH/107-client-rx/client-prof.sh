#!/bin/bash
# client-prof.sh <n>: FLAMEGRAPH round 107, the client's receive path
# (Mike, 17 November: "profile the client's receive path").  Round 106's
# profile (t2-r860) put the READ limit at the client: 83 to 84% busy, 40%
# of its cycles in memcpy, copying READ data out of skbs in the RPC
# receive worker.  This runs the READ strobe (104-rx-cpus/client-read.sh,
# files kept) at the settled setting and only reads the client, never
# changes it.  In the load, by the wire's per-second rows:
#   from second 6, 5 s of perf stat on each client socket: memory reads
#     and writes (uncore_imc cas_count), UPI data flits, inbound I/O
#     requests hitting and missing the L3 (unc_cha_tor_inserts.io_hit and
#     io_miss, DDIO), cycles, instructions, and retired loads that missed
#     the L3 by where they were served (local DRAM, remote DRAM, a remote
#     cache, a remote cache's modified line);
#   from second 12, 8 s of perf record -a -g at 999 Hz, reported by
#     symbol with and without children, by CPU, by command and CPU, and
#     memcpy's samples by CPU;
#   client-read.sh's own 5 s profile and mpstat from second 21 (ROW=21).
# Into $L/t2-r<n>/cli107-*.txt.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?run number}; O=$L/t2-r$n; mkdir -p $O || exit 1
H=$(cd ../FLAMEGRAPH/104-rx-cpus && pwd)
EV=uncore_imc/cas_count_read/,uncore_imc/cas_count_write/,unc_upi_txl_flits.all_data,unc_cha_tor_inserts.io_hit,unc_cha_tor_inserts.io_miss,cycles,instructions,mem_load_retired.l3_miss,mem_load_l3_miss_retired.local_dram,mem_load_l3_miss_retired.remote_dram,mem_load_l3_miss_retired.remote_fwd,mem_load_l3_miss_retired.remote_hitm
row() { W=$O/run/wire-sec.tsv; for i in $(seq 900); do [ -f $W ] && [ $(wc -l < $W) -ge $1 ] && return 0; sleep 0.2; done; return 1; }
(
	row 6 || exit 0
	$SSHC "perf stat -a --per-socket -x, -e $EV -- sleep 5" > $O/cli107-stat.txt 2>&1
	row 12 || exit 0
	$SSHC "perf record -q -a -g -F 999 -o /dev/shm/zc-cli107.data -- sleep 8 > /dev/null 2>&1"
	R="perf report -i /dev/shm/zc-cli107.data"
	$SSHC "$R --children -g none --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli107-children.txt
	$SSHC "$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli107-self.txt
	$SSHC "$R --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep ." > $O/cli107-cpu.txt
	$SSHC "$R --no-children -g none --sort comm,cpu --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli107-comm-cpu.txt
	$SSHC "$R --no-children -g none --sort cpu --symbols __pi_memcpy 2>/dev/null | grep -v '^#' | grep ." > $O/cli107-memcpy-cpu.txt
	$SSHC "$R --no-children -g caller,0.5,callee --sort sym --symbols __pi_memcpy 2>/dev/null | grep -v '^#' | grep . | head -80" > $O/cli107-memcpy-callers.txt
	$SSHC "rm -f /dev/shm/zc-cli107.data"
) &
KEEP_FILES=1 ROW=21 $H/client-read.sh $n; rc=$?
wait
exit $rc
