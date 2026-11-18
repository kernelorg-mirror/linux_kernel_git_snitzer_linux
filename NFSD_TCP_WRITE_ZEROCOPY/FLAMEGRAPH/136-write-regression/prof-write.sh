#!/bin/bash
# prof-write.sh <n>: FLAMEGRAPH round 136, one WRITE pass profiled and
# traced on both hosts (Mike, 18 November: "have you collected perf data
# from both the client and server while running the tip on the server?
# as the comprehensive 'before' profile and trace data").  The same
# script runs on build 541's client (before) and build 469's (after
# Mike's reboot), so the two line up.  The pass is write-check.sh <n>
# (a trim, 30 s of rest, WARM=3 RUNTIME=15 on all eight drives), and in
# its load, by the wire's per-second rows:
#   second 4: 5 s of mpstat on both hosts and of the client's memory
#     controllers (round 107's shape); nstat on both zeroed;
#   seconds 6, 9, 12: the server's and the client's TCP sockets on the
#     ports (ss -tni: Send-Q, cwnd, unacked, pacing) and the client's
#     per-transport RPC queues (xprt_info of every transport);
#   second 9: 5 s of perf record -a -g at 499 Hz on both hosts at once,
#     -N (no build-id cache), the data kept in each host's /dev/shm as
#     zc-p136-<n>.data and zc-cli136-<n>.data;
#   second 14: nstat's deltas on both.
# Into $L/t2-r<n>/: srv-, cli- mpstat, perf {self,children,comm}, ss-*,
# xprt-*, nstat-*; cli-mem.txt.  Only reads the client.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-write.sh <n>}
O=$L/t2-r$n; D=/dev/shm/zc-p136-$n.data; C=/dev/shm/zc-cli136-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
snap() {	# snap <tag>
	ss -tni '( sport = :2049 )' > $O/ss-srv-$1.txt 2>&1
	$SSHC "ss -tni '( dport = :2049 )'" > $O/ss-cli-$1.txt 2>&1
	$SSHC 'for f in /sys/kernel/sunrpc/xprt-switches/*/xprt-*/xprt_info; do echo "== $f $(cat ${f%/*}/dstaddr 2>/dev/null)"; cat $f; done' > $O/xprt-$1.txt 2>&1
}
(
	row() { W=$O/run/wire-sec.tsv; for i in $(seq 3000); do [ -f $W ] && [ $(wc -l < $W) -ge $1 ] && return 0; sleep 0.2; done; return 1; }
	row 4 || exit 0
	nstat -n; $SSHC "nstat -n"
	$SSHC "mpstat -P ALL 1 5" > $O/cli-mpstat-w.txt 2>&1 &
	mpstat -P ALL 1 5 > $O/srv-mpstat-w.txt 2>&1 &
	$SSHC "perf stat -a --per-socket -x, -e uncore_imc/cas_count_read/,uncore_imc/cas_count_write/ -- sleep 5" > $O/cli-mem.txt 2>&1 &
	row 6 && snap 6
	row 9 || exit 0
	$SSHC "perf record -q -N -a -g -F 499 -o $C -- sleep 5 > /dev/null 2>&1" &
	perf record -q -N -a -g -F 499 -o $D -- sleep 5 > /dev/null 2>&1 &
	snap 9
	row 12 && snap 12
	wait
	row 14
	nstat -a > $O/nstat-srv.txt 2>&1; $SSHC "nstat -a" > $O/nstat-cli.txt 2>&1
) &
./write-check.sh $n > $O/write-check.out 2>&1; rc=$?
wait
R="perf report -i $D --stdio"
$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/srv-self.txt
$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250 > $O/srv-children.txt
$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/srv-comm.txt
R="perf report -i $C --stdio"
$SSHC "$R --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200" > $O/cli-self.txt
$SSHC "$R --children -g none --sort sym --percent-limit 0.3 2>/dev/null | grep -v '^#' | grep . | head -250" > $O/cli-children.txt
$SSHC "$R --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60" > $O/cli-comm.txt
$SSHC "uname -r; cat /sys/module/sunrpc/srcversion" > $O/cli-kernel.txt
grep "t2-r$n" $O/write-check.out | tail -1
exit $rc
